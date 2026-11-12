#pragma once
// Backlight-aware exposure control (issue #137).
//
// When a bright window sits behind the user, the camera's own auto exposure
// meters the whole frame and leaves the face nearly black. The recognizer
// closes a small loop around the camera instead: meter the face (or the frame
// centre before a face is found) and nudge one camera control until the face
// sits in a usable brightness band — in both directions, so a face that gets
// too bright or clips is pulled back down. The background (window) is allowed
// to blow out; only the face matters for recognition.
//
// Plain header (no module, no windows.h) so it is shared by the camera module,
// the Media Foundation TU and the unit tests.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace su::recognizer::detail {

// Rectangle in frame pixel coordinates.
struct PixelRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const PixelRegion&) const = default;
};

// Camera control the closed loop drives, in order of preference. ev_bias and
// backlight_compensation keep the camera's own auto exposure running (it
// still follows lighting changes); manual_exposure turns it off, so the loop
// must keep tracking the scene itself.
enum class ExposureKnobKind { ev_bias, backlight_compensation, manual_exposure };

struct ExposureKnob {
    ExposureKnobKind kind = ExposureKnobKind::backlight_compensation;
    int min = 0;
    int max = 0;
    int step = 1;
    // Level the loop starts from. For ev_bias / backlight this is the value
    // read at open (restored on release).
    int initial = 0;
};

// Probed once when the camera opens.
struct ExposureCapabilities {
    // Camera auto exposure accepts a metering region of interest.
    bool metering_region = false;
    std::optional<ExposureKnob> knob;
};

// Luma statistics of the metered region.
struct LumaStats {
    double mean = 0.0;               // 0..255
    double clipped_fraction = 0.0;   // share of samples >= clip level
    std::size_t samples = 0;
};

struct ExposureTuning {
    double target_low = 90.0;
    double target_high = 150.0;
    std::uint8_t clip_level = 250;
    double max_clipped_fraction = 0.05;
    // Observations to wait after a change so the camera can settle before
    // the next decision (prevents oscillation).
    int settle_observations = 4;
};

struct ExposureLoopState {
    int level = 0;
    int observations_since_change = 0;
};

struct ExposureStep {
    ExposureLoopState next;
    // Set only when the camera control must change.
    std::optional<int> apply_level;
};

// Region the loop meters. With a face: the inner part of the face box (drops
// hair and the background at the box edges), clamped to the frame. Without
// one: a centred window where a seated user's face normally is, so a bright
// window at the frame edge does not dominate.
inline PixelRegion metering_region(
    const int width, const int height, const std::optional<PixelRegion> face) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    const auto clamp_to_frame = [&](int x0, int y0, int x1, int y1) {
        x0 = std::clamp(x0, 0, width);
        y0 = std::clamp(y0, 0, height);
        x1 = std::clamp(x1, 0, width);
        y1 = std::clamp(y1, 0, height);
        return PixelRegion{.x = x0, .y = y0, .width = std::max(0, x1 - x0),
                           .height = std::max(0, y1 - y0)};
    };
    if (face && face->width > 0 && face->height > 0) {
        const auto inset_x = face->width * 15 / 100;
        const auto inset_y = face->height * 15 / 100;
        const auto inner = clamp_to_frame(face->x + inset_x, face->y + inset_y,
            face->x + face->width - inset_x, face->y + face->height - inset_y);
        if (inner.width > 0 && inner.height > 0) {
            return inner;
        }
    }
    const auto region_width = width * 2 / 5;
    const auto region_height = height / 2;
    const auto x0 = (width - region_width) / 2;
    const auto y0 = (height - region_height) / 2;
    return clamp_to_frame(x0, y0, x0 + region_width, y0 + region_height);
}

// Rec.601 luma statistics over `region` of a packed RGB/RGBA (or 1-channel)
// image, subsampled to at most ~kMaxSamples pixels. Returns samples == 0 for
// an empty region or a buffer too small for the claimed geometry.
inline LumaStats measure_luma(const std::span<const std::byte> pixels, const int width,
    const int height, const int channels, const PixelRegion region,
    const std::uint8_t clip_level = ExposureTuning{}.clip_level) {
    constexpr auto kMaxSamples = 4096;
    if (width <= 0 || height <= 0 || (channels != 1 && channels != 3 && channels != 4)
        || region.width <= 0 || region.height <= 0 || region.x < 0 || region.y < 0
        || region.x + region.width > width || region.y + region.height > height) {
        return {};
    }
    const auto needed = static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
        * static_cast<std::size_t>(channels);
    if (pixels.size() < needed) {
        return {};
    }
    const auto area = static_cast<long long>(region.width) * region.height;
    auto stride = 1;
    while (area / (static_cast<long long>(stride) * stride) > kMaxSamples) {
        ++stride;
    }
    auto sum = std::uint64_t{};
    auto clipped = std::size_t{};
    auto samples = std::size_t{};
    for (auto y = region.y; y < region.y + region.height; y += stride) {
        for (auto x = region.x; x < region.x + region.width; x += stride) {
            const auto offset = (static_cast<std::size_t>(y) * width + x) * channels;
            const auto r = std::to_integer<unsigned>(pixels[offset]);
            const auto g = channels == 1 ? r : std::to_integer<unsigned>(pixels[offset + 1]);
            const auto b = channels == 1 ? r : std::to_integer<unsigned>(pixels[offset + 2]);
            const auto luma = (77U * r + 150U * g + 29U * b) >> 8U;
            sum += luma;
            clipped += luma >= clip_level ? 1U : 0U;
            ++samples;
        }
    }
    return LumaStats{
        .mean = static_cast<double>(sum) / static_cast<double>(samples),
        .clipped_fraction = static_cast<double>(clipped) / static_cast<double>(samples),
        .samples = samples,
    };
}

enum class ExposureDirection { raise, lower, hold };

// Largest single multiplicative move of a manual exposure time, and the extra
// back-off applied when the face clips (a mean-based ratio alone would not
// lower a clipped but mid-bright face).
inline constexpr double kMaxExposureRatio = 2.0;
inline constexpr double kMinExposureRatio = 0.5;
inline constexpr double kClipBackoffRatio = 0.8;

inline ExposureDirection exposure_direction(const LumaStats& stats, const ExposureTuning& tuning) {
    if (stats.clipped_fraction > tuning.max_clipped_fraction || stats.mean > tuning.target_high) {
        return ExposureDirection::lower;
    }
    return stats.mean < tuning.target_low ? ExposureDirection::raise : ExposureDirection::hold;
}

// Level the loop wants next, before clamping. Additive knobs move one step;
// exposure time is linear in brightness, so it moves proportionally toward
// the middle of the band, but always by at least one step so it cannot stall.
inline int exposure_candidate_level(const int level, const ExposureDirection direction,
    const LumaStats& stats, const ExposureKnob& knob, const ExposureTuning& tuning) {
    const auto step = std::max(1, knob.step);
    const auto sign = direction == ExposureDirection::raise ? 1 : -1;
    if (knob.kind != ExposureKnobKind::manual_exposure) {
        return level + sign * step;
    }
    const auto target_mid = (tuning.target_low + tuning.target_high) / 2.0;
    auto ratio = std::clamp(
        target_mid / std::max(stats.mean, 1.0), kMinExposureRatio, kMaxExposureRatio);
    if (direction == ExposureDirection::lower) {
        ratio = std::min(ratio, kClipBackoffRatio);
    }
    const auto scaled = static_cast<double>(level) * ratio / static_cast<double>(step);
    const auto rounded = static_cast<int>(std::llround(scaled)) * step;
    return direction == ExposureDirection::raise ? std::max(rounded, level + step)
                                                 : std::min(rounded, level - step);
}

// One decision of the exposure loop. Pure: the caller applies `apply_level`
// to the camera and keeps `next` for the following observation.
inline ExposureStep next_exposure_step(const ExposureLoopState& state, const LumaStats& stats,
    const ExposureKnob& knob, const ExposureTuning& tuning = {}) {
    const auto observations = state.observations_since_change + 1;
    const auto unchanged = ExposureStep{
        .next = {.level = state.level, .observations_since_change = observations},
        .apply_level = std::nullopt};
    if (stats.samples == 0 || observations < tuning.settle_observations) {
        return unchanged;
    }
    const auto direction = exposure_direction(stats, tuning);
    if (direction == ExposureDirection::hold) {
        return unchanged;
    }
    const auto wanted = std::clamp(
        exposure_candidate_level(state.level, direction, stats, knob, tuning), knob.min, knob.max);
    if (wanted == state.level) {
        return unchanged;
    }
    return ExposureStep{
        .next = {.level = wanted, .observations_since_change = 0}, .apply_level = wanted};
}

}  // namespace su::recognizer::detail
