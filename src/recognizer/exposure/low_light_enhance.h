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

// Gamma LUT lifting `centre.mean` toward `target_mean`, or nullopt when the
// centre is bright enough (or unmeasured) and enhancement would only add risk.
inline std::optional<LowLightLut> low_light_lut(const LumaStats& centre,
    const double mean_threshold = kLowLightMeanThreshold,
    const double target_mean = kLowLightTargetMean) {
    if (centre.samples == 0 || !(centre.mean < mean_threshold)) {
        return std::nullopt;
    }
    const auto mean = std::max(centre.mean, kLowLightMinMean);
    const auto gamma = std::clamp(std::log(target_mean / kLumaMax) / std::log(mean / kLumaMax),
        kLowLightMinGamma, kLowLightMaxGamma);
    auto lut = LowLightLut{};
    for (auto i = std::size_t{}; i < lut.size(); ++i) {
        const auto mapped = std::pow(static_cast<double>(i) / kLumaMax, gamma) * kLumaMax;
        lut[i] = static_cast<std::uint8_t>(std::clamp(std::lround(mapped), 0L, 255L));
    }
    return lut;
}

// Returns a remapped copy; the input is never modified.
inline std::vector<unsigned char> apply_lut(
    const std::span<const unsigned char> pixels, const LowLightLut& lut) {
    auto out = std::vector<unsigned char>(pixels.size());
    std::ranges::transform(pixels, out.begin(), [&](const unsigned char value) {
        return static_cast<unsigned char>(lut[value]);
    });
    return out;
}

}  // namespace su::recognizer::detail
