module;
#include <libyuv.h>
#include <linux/videodev2.h>
#include <cstddef>

module su.recognizer.image;

namespace su::recognizer {

namespace {

constexpr int kRgbChannels = 3;
constexpr int kArgbChannels = 4;

// libyuv's ARGB buffer is laid out in memory as B, G, R, A (the uint32_t is
// read as ARGB on a little-endian machine). Pull R, G, B out of that layout
// into an interleaved RGB24 buffer.
std::optional<std::size_t> checked_image_size(int width, int height, int channels) {
    if (width <= 0 || height <= 0 || channels <= 0) {
        return std::nullopt;
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const auto c = static_cast<std::size_t>(channels);
    if (w > std::numeric_limits<std::size_t>::max() / h
        || w * h > std::numeric_limits<std::size_t>::max() / c) {
        return std::nullopt;
    }
    return w * h * c;
}

void argb_to_rgb(const uint8_t* argb, std::size_t pixel_count, std::vector<std::byte>& out) {
    out.resize(pixel_count * kRgbChannels);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const auto src = i * kArgbChannels;
        const auto dst = i * kRgbChannels;
        out[dst + 0] = std::byte{argb[src + 2]};  // R
        out[dst + 1] = std::byte{argb[src + 1]};  // G
        out[dst + 2] = std::byte{argb[src + 0]};  // B
    }
}

}  // namespace

std::expected<std::vector<std::byte>, RecognizerError> v4l2_frame_to_rgb(
    const CapturedFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.bytes.empty()) {
        return std::unexpected(RecognizerError::kInvalidImage);
    }

    const auto rgb_size = checked_image_size(frame.width, frame.height, kRgbChannels);
    const auto argb_size = checked_image_size(frame.width, frame.height, kArgbChannels);
    if (!rgb_size || !argb_size
        || static_cast<std::size_t>(frame.width) > static_cast<std::size_t>(std::numeric_limits<int>::max() / kArgbChannels)) {
        return std::unexpected(RecognizerError::kInvalidImage);
    }
    const auto width = frame.width;
    const auto height = frame.height;
    const auto argb_stride = width * kArgbChannels;
    auto argb = std::vector<std::byte>(*argb_size);

    if (frame.v4l2_format == V4L2_PIX_FMT_YUYV) {
        if ((width % 2) != 0) {
            return std::unexpected(RecognizerError::kInvalidImage);
        }
        const auto minimum_stride = static_cast<std::size_t>(width) * 2;
        const auto src_stride = frame.stride == 0 ? minimum_stride : frame.stride;
        if (src_stride < minimum_stride
            || src_stride > static_cast<std::size_t>(std::numeric_limits<int>::max())
            || src_stride > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(height)
            || frame.bytes.size() < src_stride * static_cast<std::size_t>(height)) {
            return std::unexpected(RecognizerError::kInvalidImage);
        }
        const auto* src = reinterpret_cast<const uint8_t*>(frame.bytes.data());
        auto* dst = reinterpret_cast<uint8_t*>(argb.data());
        if (libyuv::YUY2ToARGB(src, static_cast<int>(src_stride), dst, argb_stride, width, height) != 0) {
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
    argb_to_rgb(reinterpret_cast<const uint8_t*>(argb.data()), *rgb_size / kRgbChannels, rgb);
    return rgb;
}

}  // namespace su::recognizer
