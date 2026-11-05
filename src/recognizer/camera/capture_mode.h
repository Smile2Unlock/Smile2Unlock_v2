#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <tuple>

namespace su::recognizer::detail {

enum class CaptureFormat { mjpeg, yuy2, nv12 };

struct CaptureMode {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rate_numerator = 0;
    std::uint32_t rate_denominator = 0;
    CaptureFormat format = CaptureFormat::yuy2;

    bool valid() const {
        return width > 0 && height > 0
            && (format == CaptureFormat::mjpeg || width % 2 == 0)
            && (format != CaptureFormat::nv12 || height % 2 == 0);
    }

    double fps() const {
        return rate_denominator == 0 ? 0.0
            : static_cast<double>(rate_numerator) / rate_denominator;
    }
};

// Recognition does not need a 4K stream. Choose a usable capture cadence
// first, then a modest frame size, favoring MJPEG and packed YUY2 over NV12
// at equal cadence and size.
// Keep the native rational rate (e.g. 30000/1001), not a guessed 30 fps type.
inline auto capture_mode_rank(const CaptureMode& mode) {
    const auto fps = mode.fps();
    const auto pixels = static_cast<std::uint64_t>(mode.width) * mode.height;
    const auto size_group = mode.width < 320 || mode.height < 240 ? 2
        : mode.width <= 1280 && mode.height <= 720 ? 0 : 1;
    constexpr auto preferred_pixels = std::uint64_t{1280} * 720;
    const auto distance = pixels > preferred_pixels ? pixels - preferred_pixels
                                                   : preferred_pixels - pixels;
    const auto format_priority = mode.format == CaptureFormat::mjpeg ? 0
        : mode.format == CaptureFormat::yuy2 ? 1 : 2;
    // Treat 29.97 and 30 as the same cadence when choosing transport/size.
    return std::tuple{fps < 14.5, size_group, -std::min(std::round(fps), 30.0),
                      format_priority, distance, std::abs(fps - 30.0)};
}

} // namespace su::recognizer::detail
