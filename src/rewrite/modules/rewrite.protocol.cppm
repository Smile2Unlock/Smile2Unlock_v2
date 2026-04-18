export module rewrite.protocol;

import std;

export namespace rewrite::ipc {

inline constexpr std::uint32_t kMagic = 0x53324950U;
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::uint16_t kHeaderBytes = 64;
inline constexpr std::size_t kAuthTagBytes = 16;
inline constexpr std::uint32_t kMaxControlBodyBytes = 128U * 1024U;
inline constexpr std::uint32_t kMaxResponseBodyBytes = 256U * 1024U;
inline constexpr std::uint32_t kMaxCaptureBodyBytes = 2U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxEventBodyBytes = 64U * 1024U;
inline constexpr std::uint32_t kMaxPreviewFrameBytes = 1536U * 1024U;
inline constexpr std::uint32_t kMaxFeatureBytes = 64U * 1024U;

enum class MessageType : std::uint16_t {
    control_request = 1,
    control_response = 2,
    event_message = 3,
    fr_command = 4,
    fr_response = 5,
    preview_frame = 6,
    capture_result = 7,
    error_message = 8
};

enum class BodyCodec : std::uint16_t {
    utf8_json = 1,
    binary_struct = 2,
    jpeg = 3,
    float32_le = 4,
    raw_bytes = 5
};

enum class StatusCode : std::uint32_t {
    ok = 0,
    accepted = 1,
    bad_magic = 100,
    version_mismatch = 101,
    bad_payload = 102,
    unauthorized = 103,
    too_large = 104,
    unknown_type = 105,
    busy = 200,
    not_found = 201,
    conflict = 202,
    internal_error = 500
};

enum class Flag : std::uint16_t {
    request = 0x0001,
    response = 0x0002,
    event = 0x0004,
    error = 0x0008,
    stream_frame = 0x0010,
    stream_end = 0x0020
};

enum class EventKind : std::uint16_t {
    camera_state = 1,
    preview_started = 2,
    preview_stopped = 3,
    recognition_status = 4,
    auth_result = 5,
    backend_error = 6
};

enum class FrCommand : std::uint16_t {
    start_preview = 1,
    stop_preview = 2,
    capture = 3,
    start_recognition = 4,
    stop_recognition = 5
};

struct FrameHeaderV1 {
    std::uint32_t magic{kMagic};
    std::uint16_t version{kVersion};
    std::uint16_t header_bytes{kHeaderBytes};
    std::uint16_t msg_type{0};
    std::uint16_t flags{0};
    std::uint32_t status{0};
    std::uint64_t request_id{0};
    std::uint64_t session_id{0};
    std::uint32_t body_bytes{0};
    std::uint16_t body_codec{0};
    std::uint16_t reserved0{0};
    std::array<std::uint8_t, kAuthTagBytes> auth_tag{};
    std::uint32_t reserved1{0};
    std::uint32_t reserved2{0};
};

static_assert(sizeof(FrameHeaderV1) == kHeaderBytes);

struct PreviewFrameMetaV1 {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t stride{0};
    std::uint16_t pixel_format{1};
    std::uint16_t compression{1};
    std::uint32_t frame_no{0};
    std::uint64_t capture_ts_ms{0};
    std::uint32_t image_bytes{0};
};

struct CaptureResultMetaV1 {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint16_t image_codec{1};
    std::uint16_t feature_codec{4};
    std::uint32_t image_bytes{0};
    std::uint32_t feature_bytes{0};
    std::uint32_t feature_dim{0};
    std::int32_t capture_status{0};
};

[[nodiscard]] inline constexpr std::uint16_t flag_value(const Flag flag) noexcept {
    return static_cast<std::uint16_t>(flag);
}

[[nodiscard]] inline std::uint16_t join_flags(std::initializer_list<Flag> flags) noexcept {
    std::uint16_t value = 0;
    for (const Flag flag : flags) {
        value = static_cast<std::uint16_t>(value | flag_value(flag));
    }
    return value;
}

[[nodiscard]] inline std::vector<std::byte> serialize_header(const FrameHeaderV1& header) {
    std::vector<std::byte> bytes(sizeof(FrameHeaderV1));
    std::memcpy(bytes.data(), &header, sizeof(FrameHeaderV1));
    return bytes;
}

[[nodiscard]] inline std::expected<FrameHeaderV1, std::string> parse_header(
    const std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(FrameHeaderV1)) {
        return std::unexpected("frame too small");
    }

    FrameHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(FrameHeaderV1));
    if (header.magic != kMagic) {
        return std::unexpected("bad magic");
    }
    if (header.version != kVersion) {
        return std::unexpected("version mismatch");
    }
    if (header.header_bytes != kHeaderBytes) {
        return std::unexpected("header size mismatch");
    }
    return header;
}

[[nodiscard]] inline std::string make_json_kv(
    std::initializer_list<std::pair<std::string_view, std::string_view>> items) {
    std::string json = "{";
    bool first = true;
    for (const auto& [key, value] : items) {
        if (!first) {
            json += ",";
        }
        first = false;
        json += std::format("\"{}\":\"{}\"", key, value);
    }
    json += "}";
    return json;
}

} // namespace rewrite::ipc
