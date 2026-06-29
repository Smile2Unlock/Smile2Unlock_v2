#pragma once

#include "recognizer/recognizer_service.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace su::recognizer {

// A captured camera frame in its native V4L2 pixel format. The bytes are
// non-owning (they point at the mmap'ed V4L2 buffer) and only valid until the
// next grab_frame call.
struct CapturedFrame {
    int width = 0;
    int height = 0;
    uint32_t v4l2_format = 0;  // V4L2_PIX_FMT_YUYV / V4L2_PIX_FMT_MJPEG
    std::span<const std::byte> bytes;
};

// Convert a captured V4L2 frame to interleaved 3-channel RGB, the pixel layout
// SeetaFace accepts. Pure function: input frame, output owned RGB buffer of
// size width * height * 3. Returns kInvalidImage for unsupported formats.
std::expected<std::vector<std::byte>, RecognizerError> v4l2_frame_to_rgb(
    const CapturedFrame& frame);

}  // namespace su::recognizer
