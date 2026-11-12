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
#include <mdspan>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

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

[[nodiscard]] constexpr std::string_view to_string(const ExposureKnobKind kind) {
    switch (kind) {
    case ExposureKnobKind::ev_bias: return "ev_bias";
    case ExposureKnobKind::backlight_compensation: return "backlight_compensation";
    case ExposureKnobKind::manual_exposure: return "manual_exposure";
    }
    std::unreachable();
}

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
[[nodiscard]] constexpr PixelRegion metering_region(
    const int width, const int height, const std::optional<PixelRegion> face) {
    constexpr auto kFaceInsetPercent = 15;
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
    const auto inner_face = [&](const PixelRegion& box) {
        const auto inset_x = box.width * kFaceInsetPercent / 100;
        const auto inset_y = box.height * kFaceInsetPercent / 100;
        return clamp_to_frame(box.x + inset_x, box.y + inset_y,
            box.x + box.width - inset_x, box.y + box.height - inset_y);
    };
    const auto is_usable = [](const PixelRegion& region) {
        return region.width > 0 && region.height > 0;
    };
    const auto centre = [&] {
        const auto region_width = width * 2 / 5;
        const auto region_height = height / 2;
        const auto x0 = (width - region_width) / 2;
        const auto y0 = (height - region_height) / 2;
        return clamp_to_frame(x0, y0, x0 + region_width, y0 + region_height);
    };
    return face.and_then([&](const PixelRegion& box) {
                   return is_usable(box) ? std::optional(inner_face(box)) : std::nullopt;
               })
        .and_then([&](const PixelRegion& inner) {
            return is_usable(inner) ? std::optional(inner) : std::nullopt;
        })
        .value_or(centre());
}

// Running totals while scanning the metered region.
struct LumaTotals {
    std::uint64_t sum = 0;
    std::size_t clipped = 0;
    std::size_t samples = 0;
};

// Rec.601 luma statistics over `region` of a packed RGB/RGBA (or 1-channel)
// image, subsampled to at most ~kMaxSamples pixels. Returns samples == 0 for
// an empty region or a buffer too small for the claimed geometry.
[[nodiscard]] constexpr LumaStats measure_luma(const std::span<const std::byte> pixels,
    const int width, const int height, const int channels, const PixelRegion region,
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
    // Row-major [row, column, channel] view of the packed buffer.
    const auto image = std::mdspan(pixels.data(), static_cast<std::size_t>(height),
        static_cast<std::size_t>(width), static_cast<std::size_t>(channels));
    const auto luma_at = [&](const int y, const int x) {
        const auto row = static_cast<std::size_t>(y);
        const auto column = static_cast<std::size_t>(x);
        const auto red = std::to_integer<unsigned>(image[row, column, 0]);
        const auto green = channels == 1 ? red : std::to_integer<unsigned>(image[row, column, 1]);
        const auto blue = channels == 1 ? red : std::to_integer<unsigned>(image[row, column, 2]);
        return (77U * red + 150U * green + 29U * blue) >> 8U;
    };
    const auto rows = std::views::iota(region.y, region.y + region.height) | std::views::stride(stride);
    const auto columns = std::views::iota(region.x, region.x + region.width) | std::views::stride(stride);
    const auto totals = std::ranges::fold_left(std::views::cartesian_product(rows, columns),
        LumaTotals{}, [&](const LumaTotals& so_far, const auto position) {
            const auto [y, x] = position;
            const auto luma = luma_at(y, x);
            return LumaTotals{.sum = so_far.sum + luma,
                .clipped = so_far.clipped + (luma >= clip_level ? 1U : 0U),
                .samples = so_far.samples + 1};
        });
    return LumaStats{
        .mean = static_cast<double>(totals.sum) / static_cast<double>(totals.samples),
        .clipped_fraction = static_cast<double>(totals.clipped) / static_cast<double>(totals.samples),
        .samples = totals.samples,
    };
}

// Which way a decision moves the camera control; hold is "no direction".
// The underlying value is the sign applied to the step.
enum class ExposureDirection : int { lower = -1, raise = 1 };

// Largest single multiplicative move of a manual exposure time, and the extra
// back-off applied when the face clips (a mean-based ratio alone would not
// lower a clipped but mid-bright face).
inline constexpr double kMaxExposureRatio = 2.0;
inline constexpr double kMinExposureRatio = 0.5;
inline constexpr double kClipBackoffRatio = 0.8;

[[nodiscard]] constexpr std::optional<ExposureDirection> exposure_direction(
    const LumaStats& stats, const ExposureTuning& tuning) {
    if (stats.clipped_fraction > tuning.max_clipped_fraction || stats.mean > tuning.target_high) {
        return ExposureDirection::lower;
    }
    if (stats.mean < tuning.target_low) {
        return ExposureDirection::raise;
    }
    return std::nullopt;
}

// Level the loop wants next, before clamping. Additive knobs move one step;
// exposure time is linear in brightness, so it moves proportionally toward
// the middle of the band, but always by at least one step so it cannot stall.
[[nodiscard]] constexpr int exposure_candidate_level(const int level,
    const ExposureDirection direction, const LumaStats& stats, const ExposureKnob& knob,
    const ExposureTuning& tuning) {
    const auto step = std::max(1, knob.step);
    const auto signed_step = std::to_underlying(direction) * step;
    if (knob.kind != ExposureKnobKind::manual_exposure) {
        return level + signed_step;
    }
    const auto target_mid = (tuning.target_low + tuning.target_high) / 2.0;
    const auto ratio = std::clamp(
        target_mid / std::max(stats.mean, 1.0), kMinExposureRatio, kMaxExposureRatio);
    const auto directed_ratio = direction == ExposureDirection::lower
        ? std::min(ratio, kClipBackoffRatio)
        : ratio;
    const auto scaled = static_cast<double>(level) * directed_ratio / static_cast<double>(step);
    const auto rounded = static_cast<int>(std::llround(scaled)) * step;
    return direction == ExposureDirection::raise ? std::max(rounded, level + step)
                                                 : std::min(rounded, level - step);
}

// One decision of the exposure loop. Pure: the caller applies `apply_level`
// to the camera and keeps `next` for the following observation.
[[nodiscard]] constexpr ExposureStep next_exposure_step(const ExposureLoopState& state,
    const LumaStats& stats, const ExposureKnob& knob, const ExposureTuning& tuning = {}) {
    const auto observations = state.observations_since_change + 1;
    const auto unchanged = ExposureStep{
        .next = {.level = state.level, .observations_since_change = observations},
        .apply_level = std::nullopt};
    if (stats.samples == 0 || observations < tuning.settle_observations) {
        return unchanged;
    }
    return exposure_direction(stats, tuning)
        .transform([&](const ExposureDirection direction) {
            return std::clamp(exposure_candidate_level(state.level, direction, stats, knob, tuning),
                knob.min, knob.max);
        })
        .and_then([&](const int wanted) {
            return wanted == state.level ? std::nullopt : std::optional(wanted);
        })
        .transform([](const int wanted) {
            return ExposureStep{
                .next = {.level = wanted, .observations_since_change = 0}, .apply_level = wanted};
        })
        .value_or(unchanged);
}

}  // namespace su::recognizer::detail
