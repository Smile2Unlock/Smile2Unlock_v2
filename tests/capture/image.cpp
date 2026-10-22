#include "common/capture/image.h"
#include <cstdlib>
#include <iostream>

void check(bool value) { if (!value) std::abort(); }
int main() {
    using su::capture::prepare_image;
    check(!prepare_image({}, 0, 480));
    check(!prepare_image({}, 640, 480));
    auto frame = std::vector<std::byte>(640 * 480 * 3, std::byte{0x80});
    const auto jpeg = prepare_image(frame, 640, 480);
    check(jpeg && jpeg->width == 320 && jpeg->height == 240);
    check(jpeg->bytes.size() <= su::capture::kMaximumJpegBytes);
    check(jpeg->bytes.front() == 0xff && jpeg->bytes[1] == 0xd8);
    check(jpeg->bytes[jpeg->bytes.size() - 2] == 0xff && jpeg->bytes.back() == 0xd9);
    const auto portrait = prepare_image(frame, 480, 640);
    check(portrait && portrait->width == 240 && portrait->height == 320);
    const auto small = prepare_image(std::span(frame).first(30 * 20 * 3), 30, 20);
    check(small && small->width == 30 && small->height == 20);
    std::cout << "bounded JPEG preparation passed\n";
}
