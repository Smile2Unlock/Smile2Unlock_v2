module;

#include <expected>

export module su.app.services.backend;

import std;
import su.app.state;

export namespace su::app::services {

enum class BackendError { kUserNotFound, kFaceNotFound, kServiceUnavailable };

class IBackendService {
public:
    virtual ~IBackendService() = default;

    virtual std::vector<UserEntry> get_users() = 0;
    virtual int add_user(std::string_view username, std::string_view remark) = 0;
    virtual std::expected<void, BackendError> delete_user(int user_id) = 0;
    virtual std::expected<void, BackendError> capture_and_add_face(int user_id) = 0;
    virtual std::expected<void, BackendError> delete_face(int user_id, int face_id) = 0;
    virtual std::vector<CameraInfo> enumerate_cameras() = 0;
    virtual std::expected<void, BackendError> start_recognition(int camera_index) = 0;
    virtual std::expected<void, BackendError> stop_recognition() = 0;
    virtual std::expected<void, BackendError> start_preview(int camera_index, int fps = 15) = 0;
    virtual std::expected<void, BackendError> stop_preview() = 0;

    virtual AppConfig load_config() = 0;
    virtual std::expected<void, BackendError> save_config(const AppConfig& config) = 0;
};

class MockBackendService final : public IBackendService {
public:
    std::vector<UserEntry> get_users() override {
        return {
            {.user_id = 1, .username = "Alice", .remark = "Admin", .face_count = 2},
            {.user_id = 2, .username = "Bob", .remark = "Staff", .face_count = 2},
            {.user_id = 3, .username = "Carol", .remark = "Guest", .face_count = 1},
        };
    }

    int add_user(std::string_view username, std::string_view remark) override {
        const auto id = next_user_id_++;
        users_.push_back(UserEntry{
            .user_id = id,
            .username = std::string(username),
            .remark = std::string(remark),
            .face_count = 0,
        });
        return id;
    }

    std::expected<void, BackendError> delete_user(int user_id) override {
        if (user_id <= 0) {
            return std::unexpected(BackendError::kUserNotFound);
        }
        std::erase_if(users_, [&](const auto& u) { return u.user_id == user_id; });
        return {};
    }

    std::expected<void, BackendError> capture_and_add_face(int user_id) override {
        (void)user_id;
        return {};
    }

    std::expected<void, BackendError> delete_face(int user_id, int face_id) override {
        (void)user_id;
        (void)face_id;
        return {};
    }

    std::vector<CameraInfo> enumerate_cameras() override {
        return {
            {.index = 0, .name = "Built-in Camera (V4L2)"},
            {.index = 1, .name = "USB Camera 1080p"},
        };
    }

    std::expected<void, BackendError> start_recognition(int camera_index) override {
        (void)camera_index;
        return {};
    }

    std::expected<void, BackendError> stop_recognition() override {
        return {};
    }

    std::expected<void, BackendError> start_preview(int camera_index, int fps) override {
        (void)camera_index;
        (void)fps;
        return {};
    }

    std::expected<void, BackendError> stop_preview() override {
        return {};
    }

    AppConfig load_config() override {
        return AppConfig{
            .selected_camera = 0,
            .recognition_threshold = 0.65f,
            .liveness_detection = true,
        };
    }

    std::expected<void, BackendError> save_config(const AppConfig& config) override {
        (void)config;
        return {};
    }

private:
    int next_user_id_ = 100;
    std::vector<UserEntry> users_{};
};

inline IBackendService& backend() {
    static MockBackendService instance;
    return instance;
}

}  // namespace su::app::services
