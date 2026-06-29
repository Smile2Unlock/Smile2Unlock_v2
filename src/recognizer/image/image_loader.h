#pragma once

#include "recognizer/recognizer_service.h"

#include <filesystem>
#include <vector>

namespace su::recognizer {

// Decoded image owned by the loader. Always normalized to 3-channel RGB,
// which is one of the channel counts SeetaFace accepts (1/3/4).
struct LoadedImage {
    int width = 0;
    int height = 0;
    int channels = 3;
    std::vector<std::byte> bytes;
};

// Load and decode an image file (PNG/JPG/BMP/PPM/...) into RGB pixels.
// Returns kImageLoadFailed when the file is missing or cannot be decoded.
std::expected<LoadedImage, RecognizerError> load_image_file(
    const std::filesystem::path& path);

}  // namespace su::recognizer
