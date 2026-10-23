#pragma once
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace su::capture {
inline constexpr std::size_t kMaximumJpegBytes = 45 * 1024;
enum class ImageError { invalid, encode_failed, oversized };
struct Jpeg {
    std::vector<std::uint8_t> bytes;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
std::expected<Jpeg, ImageError> prepare_image(
    std::span<const std::byte> rgb, int width, int height);
}
