#pragma once

#include <array>
#include <cstdint>

namespace smile2unlock::recognition_agent_ipc {

inline constexpr std::uint32_t kRequestMagic = 0x53324152; // S2AR
inline constexpr std::uint32_t kResponseMagic = 0x53324150; // S2AP
inline constexpr std::uint16_t kVersion = 2;
inline constexpr std::size_t kNonceSize = 32;
inline constexpr std::size_t kMaximumFeatureCount = 4096;
inline constexpr std::size_t kMaximumJpegBytes = 45 * 1024;
inline constexpr std::uint16_t kCaptureRequested = 1;

enum class AgentStatus : std::uint32_t {
    kOk = 0,
    kInvalidRequest = 1,
    kCameraUnavailable = 2,
    kModelUnavailable = 3,
    kNoLiveFace = 4,
    kTimedOut = 5,
    kInternalError = 6,
};

struct Request {
    std::uint32_t magic = kRequestMagic;
    std::uint16_t version = kVersion;
    std::uint16_t flags = 0;
    std::array<std::uint8_t, kNonceSize> nonce{};
    std::int32_t camera_index = 0;
    std::uint32_t timeout_ms = 10'000;
    float liveness_threshold = 0.5F;
};

struct Response {
    std::uint32_t magic = kResponseMagic;
    std::uint16_t version = kVersion;
    std::uint16_t reserved = 0;
    std::array<std::uint8_t, kNonceSize> nonce{};
    AgentStatus status = AgentStatus::kInternalError;
    std::uint32_t feature_count = 0;
    float liveness_score = 0.0F;
    std::array<float, kMaximumFeatureCount> feature{};
    std::uint32_t jpeg_size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::array<std::uint8_t, kMaximumJpegBytes> jpeg{};
};

static_assert(sizeof(Request) == 52);
static_assert(sizeof(Response) == 62528);

} // namespace smile2unlock::recognition_agent_ipc
