export module su.recognizer.image;

import std;
import su.recognizer.types;

export namespace su::recognizer {

// Decoded image owned by the loader. Always normalized to 3-channel RGB.
struct LoadedImage {
    int width = 0;
    int height = 0;
    int channels = 3;
    std::vector<std::byte> bytes;
};

// Convert a captured V4L2 frame to interleaved 3-channel RGB.
std::expected<std::vector<std::byte>, RecognizerError> v4l2_frame_to_rgb(
    const CapturedFrame& frame);

// Load and decode an image file (PNG/JPG/BMP/PPM/...) into RGB pixels.
std::expected<LoadedImage, RecognizerError> load_image_file(
    const std::filesystem::path& path);

} // namespace su::recognizer
