#include "recognizer/image/image_loader.h"

#include <cstddef>
#include <cstring>

// CImg pulls in platform display backends we do not use; disable them so the
// header compiles without X11/Wayland/OpenGL dependencies.
#ifndef cimg_display
#define cimg_display 0
#endif
#include <CImg.h>

#include <exception>
#include <filesystem>

namespace su::recognizer {

namespace {

constexpr int kRgbChannels = 3;

}  // namespace

std::expected<LoadedImage, RecognizerError> load_image_file(
    const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(RecognizerError::kInvalidArgument);
    }
    if (!std::filesystem::is_regular_file(path)) {
        return std::unexpected(RecognizerError::kImageLoadFailed);
    }

    cimg_library::CImg<unsigned char> image;
    try {
        image.load(path.string().c_str());
    } catch (const cimg_library::CImgException&) {
        return std::unexpected(RecognizerError::kImageLoadFailed);
    } catch (const std::exception&) {
        return std::unexpected(RecognizerError::kImageLoadFailed);
    } catch (...) {
        return std::unexpected(RecognizerError::kImageLoadFailed);
    }

    if (image.is_empty() || image.width() <= 0 || image.height() <= 0) {
        return std::unexpected(RecognizerError::kImageLoadFailed);
    }

    const auto width = image.width();
    const auto height = image.height();
    // CImg stores channels as a separated (planar) dimension. Normalize to
    // interleaved 3-channel RGB so it maps directly onto SeetaImageData.
    if (image.spectrum() != kRgbChannels) {
        image.resize(image.width(), image.height(), 1, kRgbChannels, /*value=*/0);
    }

    auto bytes = std::vector<std::byte>(static_cast<std::size_t>(width * height * kRgbChannels));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto out_index =
                static_cast<std::size_t>((y * width + x) * kRgbChannels);
            bytes[out_index + 0] = std::byte{image(x, y, 0, 0)};  // R
            bytes[out_index + 1] = std::byte{image(x, y, 0, 1)};  // G
            bytes[out_index + 2] = std::byte{image(x, y, 0, 2)};  // B
        }
    }

    return LoadedImage{
        .width = width,
        .height = height,
        .channels = kRgbChannels,
        .bytes = std::move(bytes),
    };
}

}  // namespace su::recognizer
