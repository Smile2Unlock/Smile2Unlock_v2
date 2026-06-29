#include "recognizer/image/pixel_convert.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include <libyuv.h>
#include <linux/videodev2.h>

namespace su::recognizer {

namespace {

constexpr int kRgbChannels = 3;
constexpr int kArgbChannels = 4;

// libyuv's ARGB buffer is laid out in memory as B, G, R, A (the uint32_t is
// read as ARGB on a little-endian machine). Pull R, G, B out of that layout
// into an interleaved RGB24 buffer.
void argb_to_rgb(const uint8_t* argb, int width, int height, std::vector<std::byte>& out) {
    out.resize(static_cast<std::size_t>(width * height * kRgbChannels));
    for (int i = 0; i < width * height; ++i) {
        const auto src = i * kArgbChannels;
        const auto dst = i * kRgbChannels;
        out[dst + 0] = std::byte{argb[src + 2]};  // R
        out[dst + 1] = std::byte{argb[src + 1]};  // G
        out[dst + 2] = std::byte{argb[src + 0]};  // B
    }
}

std::vector<std::byte> argb_buffer(int width, int height) {
    return std::vector<std::byte>(static_cast<std::size_t>(width * height * kArgbChannels));
}

}  // namespace

std::expected<std::vector<std::byte>, RecognizerError> v4l2_frame_to_rgb(
    const CapturedFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.bytes.empty()) {
        return std::unexpected(RecognizerError::kInvalidImage);
    }

    const auto width = frame.width;
    const auto height = frame.height;
    auto argb = argb_buffer(width, height);
    const auto argb_stride = width * kArgbChannels;

    if (frame.v4l2_format == V4L2_PIX_FMT_YUYV) {
        const auto src_stride = width * 2;  // YUYV packs 2 pixels in 4 bytes
        const auto* src = reinterpret_cast<const uint8_t*>(frame.bytes.data());
        auto* dst = reinterpret_cast<uint8_t*>(argb.data());
        if (libyuv::YUY2ToARGB(src, src_stride, dst, argb_stride, width, height) != 0) {
            return std::unexpected(RecognizerError::kInvalidImage);
        }
    } else if (frame.v4l2_format == V4L2_PIX_FMT_MJPEG) {
        const auto sample_size = frame.bytes.size();
        const auto* sample = reinterpret_cast<const uint8_t*>(frame.bytes.data());
        auto* dst = reinterpret_cast<uint8_t*>(argb.data());
        if (libyuv::MJPGToARGB(sample, sample_size, dst, argb_stride,
                               width, height, width, height) != 0) {
            return std::unexpected(RecognizerError::kInvalidImage);
        }
    } else {
        return std::unexpected(RecognizerError::kInvalidImage);
    }

    std::vector<std::byte> rgb;
    argb_to_rgb(reinterpret_cast<const uint8_t*>(argb.data()), width, height, rgb);
    return rgb;
}

}  // namespace su::recognizer
