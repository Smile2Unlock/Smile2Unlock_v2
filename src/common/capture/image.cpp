#include "image.h"
#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <jpeglib.h>

namespace su::capture {
namespace {
struct JpegError { jpeg_error_mgr base; std::jmp_buf jump; };
void fail(j_common_ptr info) {
    auto* error = reinterpret_cast<JpegError*>(info->err);
    std::longjmp(error->jump, 1);
}
std::expected<std::vector<std::uint8_t>, ImageError> encode(
    std::span<const std::uint8_t> rgb, unsigned width, unsigned height) {
    struct State {
        jpeg_compress_struct info{};
        JpegError error{};
        unsigned char* memory = nullptr;
        unsigned long length = 0;
        ~State() { jpeg_destroy_compress(&info); std::free(memory); }
    };
    // Longjmp must not observe changed automatic non-volatile objects.
    const auto state = std::make_unique<State>();
    auto& info = state->info;
    info.err = jpeg_std_error(&state->error.base);
    state->error.base.error_exit = fail;
    if (setjmp(state->error.jump)) return std::unexpected(ImageError::encode_failed);
    jpeg_create_compress(&info);
    jpeg_mem_dest(&info, &state->memory, &state->length);
    info.image_width = width;
    info.image_height = height;
    info.input_components = 3;
    info.in_color_space = JCS_RGB;
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, 70, TRUE);
    jpeg_start_compress(&info, TRUE);
    while (info.next_scanline < info.image_height) {
        auto* row = const_cast<JSAMPLE*>(rgb.data() + info.next_scanline * width * 3);
        jpeg_write_scanlines(&info, &row, 1);
    }
    jpeg_finish_compress(&info);
    auto result = std::vector<std::uint8_t>(state->memory, state->memory + state->length);
    if (result.size() > kMaximumJpegBytes) return std::unexpected(ImageError::oversized);
    return result;
}
}
std::expected<Jpeg, ImageError> prepare_image(
    std::span<const std::byte> rgb, int width, int height) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384
        || rgb.size() != static_cast<std::size_t>(width) * height * 3)
        return std::unexpected(ImageError::invalid);
    const auto edge = std::max(width, height);
    const auto w = static_cast<unsigned>(edge <= 320 ? width : std::max(1, width * 320 / edge));
    const auto h = static_cast<unsigned>(edge <= 320 ? height : std::max(1, height * 320 / edge));
    auto scaled = std::vector<std::uint8_t>(w * h * 3);
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            const auto source = (static_cast<std::size_t>(y) * height / h * width + x * width / w) * 3;
            for (unsigned c = 0; c < 3; ++c) scaled[(y * w + x) * 3 + c] = std::to_integer<std::uint8_t>(rgb[source + c]);
        }
    }
    return encode(scaled, w, h).transform([=](auto bytes) { return Jpeg{std::move(bytes), w, h}; });
}
}
