#pragma once
// Low-light enhancement for the FACE DETECTOR ONLY (issue #137).
//
// In a backlit scene the face can be so dark (luma ~12/255) that the detector
// finds nothing, so the exposure loop never gets a face box to meter. When the
// first detection pass fails and the frame centre is dark, the detector is
// retried on a gamma-brightened copy. The enhanced pixels must never reach
// landmarking, anti-spoofing or feature extraction: enrolled templates and
// liveness scores were produced from raw frames and would shift.
//
// Plain header with pure inline functions so the backend and unit tests share it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include "recognizer/exposure/exposure_control.h"

namespace su::recognizer::detail {

using LowLightLut = std::array<std::uint8_t, 256>;

// Centre mean luma at or above which the detector already has enough light.
inline constexpr double kLowLightMeanThreshold = 70.0;
// Centre mean the gamma curve aims for: a comfortable mid-dark face level.
inline constexpr double kLowLightTargetMean = 110.0;
// Gamma bounds. The floor stops near-black frames from being amplified into
// pure noise; the ceiling guarantees the curve only ever brightens.
inline constexpr double kLowLightMinGamma = 0.3;
inline constexpr double kLowLightMaxGamma = 1.0;
// Keeps log(mean / 255) finite for an all-black centre.
inline constexpr double kLowLightMinMean = 1.0;
inline constexpr double kLumaMax = 255.0;
// Original-image mean luma of a face box below which a face found only by the
// enhanced pass is not trusted: the frame is near-black and noise-amplified,
// so landmarks, liveness and the embedding would be junk. Such a detection is
// still good enough for the exposure loop to meter.
inline constexpr double kLowLightFaceMeanFloor = 40.0;

// Output level of `gamma` at input level `index`, rounded to 0..255.
[[nodiscard]] constexpr std::uint8_t gamma_level(const int index, const double gamma) {
    const auto mapped = std::pow(static_cast<double>(index) / kLumaMax, gamma) * kLumaMax;
    return static_cast<std::uint8_t>(std::clamp(mapped + 0.5, 0.0, kLumaMax));
}

[[nodiscard]] constexpr LowLightLut gamma_lut(const double gamma) {
    auto lut = LowLightLut{};
    std::ranges::copy(std::views::iota(0, static_cast<int>(lut.size()))
            | std::views::transform([gamma](const int i) { return gamma_level(i, gamma); }),
        lut.begin());
    return lut;
}

// Gamma LUT lifting `centre.mean` toward `target_mean`, or nullopt when the
// centre is bright enough (or unmeasured) and enhancement would only add risk.
[[nodiscard]] constexpr std::optional<LowLightLut> low_light_lut(const LumaStats& centre,
    const double mean_threshold = kLowLightMeanThreshold,
    const double target_mean = kLowLightTargetMean) {
    if (centre.samples == 0 || !(centre.mean < mean_threshold)) {
        return std::nullopt;
    }
    const auto mean = std::max(centre.mean, kLowLightMinMean);
    const auto gamma = std::clamp(std::log(target_mean / kLumaMax) / std::log(mean / kLumaMax),
        kLowLightMinGamma, kLowLightMaxGamma);
    return gamma_lut(gamma);
}

// True when the original-image luma of a face box is high enough for an
// enhanced-only detection to feed landmarks, liveness and the embedding. An
// unmeasurable box (no samples) is not trusted.
[[nodiscard]] constexpr bool enhanced_face_is_usable(
    const LumaStats& face, const double mean_floor = kLowLightFaceMeanFloor) {
    return face.samples > 0 && face.mean >= mean_floor;
}

// Remaps `pixels` into `out` (same size, caller-owned so a hot path can reuse
// one scratch buffer). The input is never modified.
inline void apply_lut(const std::span<const unsigned char> pixels, const LowLightLut& lut,
    const std::span<unsigned char> out) {
    std::ranges::transform(pixels, out.begin(),
        [&lut](const unsigned char value) { return static_cast<unsigned char>(lut[value]); });
}

// Returns a remapped copy; the input is never modified.
[[nodiscard]] inline std::vector<unsigned char> apply_lut(
    const std::span<const unsigned char> pixels, const LowLightLut& lut) {
    auto remapped = std::vector<unsigned char>(pixels.size());
    apply_lut(pixels, lut, remapped);
    return remapped;
}

// Compile-time guarantees every generated LUT relies on.
namespace low_light_checks {
inline constexpr auto kDarkest = low_light_lut(LumaStats{.mean = 0.0, .samples = 1});
inline constexpr auto kDark = low_light_lut(LumaStats{.mean = 12.0, .samples = 1});
static_assert(kDarkest && kDark);
static_assert(std::ranges::is_sorted(*kDarkest) && std::ranges::is_sorted(*kDark));
static_assert((*kDarkest)[0] == 0 && (*kDarkest)[255] == 255);
static_assert((*kDark)[0] == 0 && (*kDark)[255] == 255);
static_assert((*kDark)[12] > 12);
static_assert(!low_light_lut(LumaStats{.mean = kLowLightMeanThreshold, .samples = 1}));
static_assert(!low_light_lut(LumaStats{}));
}  // namespace low_light_checks

}  // namespace su::recognizer::detail
