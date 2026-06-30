module;
// CImg pulls in platform display backends we do not use; disable them so the
// header compiles without X11/Wayland/OpenGL dependencies.
#ifndef cimg_display
#define cimg_display 0
#endif
#include <CImg.h>
#include <exception>
#include <filesystem>

module su.recognizer.image;

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

    const auto pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    auto bytes = std::vector<std::byte>(pixel_count * kRgbChannels);
    // Use mdspan for zero-overhead 3D indexing over the interleaved RGB
    // buffer: extents<height, width, 3>. This replaces the manual pointer
    // arithmetic (y * width + x) * 3 with structured [y, x, channel] access.
    auto rgb = std::mdspan<std::byte, std::dextents<std::size_t, 3>>(
        bytes.data(), height, width, kRgbChannels);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            rgb[static_cast<std::size_t>(y),
                static_cast<std::size_t>(x), 0] = std::byte{image(x, y, 0, 0)};  // R
            rgb[static_cast<std::size_t>(y),
                static_cast<std::size_t>(x), 1] = std::byte{image(x, y, 0, 1)};  // G
            rgb[static_cast<std::size_t>(y),
                static_cast<std::size_t>(x), 2] = std::byte{image(x, y, 0, 2)};  // B
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
