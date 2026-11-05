#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdint>
#if defined(__linux__)
#include <linux/videodev2.h>
#else
constexpr std::uint32_t V4L2_PIX_FMT_NV12 = 0x3231564E;
#endif

import std;
import su.recognizer.image;
import su.recognizer.types;

int main() {
    // Two padded luma rows followed by one padded interleaved chroma row.
    const std::array<std::byte, 12> padded_nv12{
        std::byte{64}, std::byte{80}, std::byte{0}, std::byte{0},
        std::byte{96}, std::byte{112}, std::byte{0}, std::byte{0},
        std::byte{90}, std::byte{240}, std::byte{0}, std::byte{0},
    };
    const auto frame = su::recognizer::CapturedFrame{
        .width = 2,
        .height = 2,
        .stride = 4,
        .v4l2_format = V4L2_PIX_FMT_NV12,
        .bytes = padded_nv12,
    };
    const auto converted = su::recognizer::v4l2_frame_to_rgb(frame);
    assert(converted);
    assert(converted->size() == 12);
    assert((*converted)[0] != (*converted)[6]);
    // Cr must contribute to red, not blue: catch swapped UV/RGB channels.
    assert(std::to_integer<int>((*converted)[0]) > 200);
    assert(std::to_integer<int>((*converted)[1]) < 20);
    assert(std::to_integer<int>((*converted)[2]) < 20);

    const std::array<std::byte, 6> packed_nv12{
        std::byte{64}, std::byte{80}, std::byte{96}, std::byte{112},
        std::byte{90}, std::byte{240},
    };
    auto packed = frame;
    packed.stride = 0;
    packed.bytes = packed_nv12;
    assert(su::recognizer::v4l2_frame_to_rgb(packed) == converted);

    const auto truncated = su::recognizer::CapturedFrame{
        .width = 2,
        .height = 2,
        .stride = 4,
        .v4l2_format = V4L2_PIX_FMT_NV12,
        .bytes = std::span<const std::byte>(padded_nv12).first(11),
    };
    assert(!su::recognizer::v4l2_frame_to_rgb(truncated));
    auto invalid = frame;
    invalid.stride = 1;
    assert(!su::recognizer::v4l2_frame_to_rgb(invalid));
    invalid.stride = std::numeric_limits<std::size_t>::max();
    assert(!su::recognizer::v4l2_frame_to_rgb(invalid));
    invalid = frame;
    invalid.height = 1;
    assert(!su::recognizer::v4l2_frame_to_rgb(invalid));
    invalid = frame;
    invalid.width = 1;
    assert(!su::recognizer::v4l2_frame_to_rgb(invalid));
    std::cout << "PASS: NV12 RGB/UV order, packed/padded stride, truncated/invalid-buffer rejection\n";
}
