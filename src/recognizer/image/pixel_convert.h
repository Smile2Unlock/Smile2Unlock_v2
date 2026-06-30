#pragma once

#include "recognizer/recognizer_service.h"

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

namespace su::recognizer {

// Convert a captured V4L2 frame to interleaved 3-channel RGB, the pixel layout
// SeetaFace accepts. Pure function: input frame, output owned RGB buffer of
// size width * height * 3. Returns kInvalidImage for unsupported formats.
std::expected<std::vector<std::byte>, RecognizerError> v4l2_frame_to_rgb(
    const CapturedFrame& frame);

}  // namespace su::recognizer
