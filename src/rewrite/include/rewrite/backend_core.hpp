#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rewrite::backend {

struct User {
    std::uint64_t id{0};
    std::string username;
    std::string password_hash;
    std::string remark;
    std::uint64_t created_at{0};
    std::uint64_t updated_at{0};
};

struct FaceRecord {
    std::uint64_t id{0};
    std::uint64_t user_id{0};
    std::vector<float> feature;
    std::string remark;
    std::uint64_t created_at{0};
};

struct RuntimeState {
    bool initialized;
    bool running;
    std::string summary;
};

struct EndpointSet {
    std::string control;
    std::string events;
    std::string fr_control;
    std::string preview;
};

struct CameraConfig {
    int device_index{0};
    int width{1280};
    int height{720};
    int max_fps{15};
    std::string codec{"jpeg"};
};

struct RecognitionConfig {
    float similarity_threshold{0.7f};
    int recognition_timeout_ms{30000};
    int capture_retry_count{3};
};

struct PreviewConfig {
    int width{1280};
    int height{720};
    int max_fps{15};
    std::string codec{"jpeg"};
};

struct BackendConfig {
    CameraConfig camera;
    RecognitionConfig recognition;
    PreviewConfig preview;
    bool debug_mode{false};
    int log_level{2}; // 0=error, 1=warn, 2=info, 3=debug
};

class BackendCore {
public:
    BackendCore();
    ~BackendCore();

    BackendCore(const BackendCore&) = delete;
    BackendCore& operator=(const BackendCore&) = delete;
    BackendCore(BackendCore&&) noexcept;
    BackendCore& operator=(BackendCore&&) noexcept;

    std::expected<void, std::string> initialize();
    std::expected<void, std::string> start_listeners();
    std::string status_json() const;

    const RuntimeState& state() const noexcept;
    const EndpointSet& endpoints() const noexcept;

    // backend::users interface
    std::expected<std::vector<User>, std::string> get_all_users();
    std::expected<User, std::string> get_user(std::uint64_t id);
    std::expected<User, std::string> add_user(std::string_view username, std::string_view password_hash, std::string_view remark);
    std::expected<User, std::string> update_user(std::uint64_t id, std::string_view username, std::string_view password_hash, std::string_view remark);
    std::expected<void, std::string> delete_user(std::uint64_t id);
    std::expected<std::vector<FaceRecord>, std::string> get_user_faces(std::uint64_t user_id);
    std::expected<FaceRecord, std::string> add_user_face(std::uint64_t user_id, std::span<const float> feature, std::string_view remark);
    std::expected<void, std::string> delete_user_face(std::uint64_t user_id, std::uint64_t face_id);

    // backend::config interface
    std::expected<BackendConfig, std::string> get_config();
    std::expected<void, std::string> set_config(const BackendConfig& config);

    // backend::recognition interface
    struct CaptureResult {
        int width{0};
        int height{0};
        std::vector<std::uint8_t> image_data;
        std::vector<float> feature;
        int capture_status{0};
    };

    struct RecognitionResult {
        std::uint64_t session_id{0};
        std::uint64_t user_id{0};
        std::string username;
        float similarity{0.0f};
        bool recognized{false};
    };

    std::expected<void, std::string> start_preview(int camera, std::string_view codec, int max_fps, int width, int height);
    std::expected<void, std::string> stop_preview();
    std::expected<CaptureResult, std::string> capture_frame(bool want_feature);
    std::expected<RecognitionResult, std::string> start_recognition(std::string_view username_hint, bool interactive);
    std::expected<void, std::string> stop_recognition(std::uint64_t session_id);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace rewrite::backend