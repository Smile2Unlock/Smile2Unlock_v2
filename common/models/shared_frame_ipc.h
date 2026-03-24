#pragma once

#include <cstdint>

namespace smile2unlock {

struct SharedFrameHeader {
    static constexpr uint32_t MAGIC = 0x5346524D; // "SFRM"
    static constexpr uint32_t VERSION = 1;
    static constexpr uint32_t MAX_IMAGE_BYTES = 8 * 1024 * 1024;

    uint32_t magic = MAGIC;
    uint32_t version = VERSION;
    int32_t status_code = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t channels = 0;
    uint32_t image_bytes = 0;
};

enum class SharedFrameStatus : int32_t {
    EMPTY = 0,
    READY = 1,
    CAPTURE_FAILED = -1,
    INVALID_FRAME = -2
};

} // namespace smile2unlock
