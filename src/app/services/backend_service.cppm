module;

#include <eui/EUINEO.h>

export module su.app.services.backend;

import std;
import su.app.state;

export namespace su::app::services {

// IBackendService: GUI-facing facade for backend operations.
// Currently backed by a mock implementation.
// When backend is ready, swap the mock for real IPC calls.
class IBackendService {
public:
    virtual ~IBackendService() = default;

    // Users
    virtual std::vector<UserEntry> get_users() = 0;
    virtual int add_user(std::string_view username, std::string_view remark) = 0;
    virtual bool delete_user(int user_id) = 0;

    // Face enrollment
    virtual bool capture_and_add_face(int user_id) = 0;
    virtual bool delete_face(int user_id, int face_id) = 0;

    // Cameras
    virtual std::vector<CameraInfo> enumerate_cameras() = 0;

    // Recognition
    virtual bool start_recognition(int camera_index) = 0;
    virtual bool stop_recognition() = 0;
    virtual bool start_preview(int camera_index, int fps = 15) = 0;
    virtual bool stop_preview() = 0;

    // Config
    virtual AppConfig load_config() = 0;
    virtual void save_config(const AppConfig& config) = 0;
};

// --- Mock implementation for GUI development ---

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
        static int next_id = 100;
        auto user = UserEntry{
            .user_id = next_id,
            .username = std::string(username),
            .remark = std::string(remark),
            .face_count = 0,
        };
        next_user_id_ = next_id++;
        return next_user_id_;
    }

    bool delete_user(int user_id) override {
        return user_id > 0;
    }

    bool capture_and_add_face(int user_id) override {
        (void)user_id;
        return true;
    }

    bool delete_face(int user_id, int face_id) override {
        (void)user_id;
        (void)face_id;
        return true;
    }

    std::vector<CameraInfo> enumerate_cameras() override {
        return {
            {.index = 0, .name = "Built-in Camera (V4L2)"},
            {.index = 1, .name = "USB Camera 1080p"},
        };
    }

    bool start_recognition(int camera_index) override {
        (void)camera_index;
        return true;
    }

    bool stop_recognition() override {
        return true;
    }

    bool start_preview(int camera_index, int fps) override {
        (void)camera_index;
        (void)fps;
        return true;
    }

    bool stop_preview() override {
        return true;
    }

    AppConfig load_config() override {
        return AppConfig{
            .selected_camera = 0,
            .recognition_threshold = 0.65f,
            .liveness_detection = true,
        };
    }

    void save_config(const AppConfig& config) override {
        (void)config;
    }

private:
    int next_user_id_ = 100;
};

// Singleton accessor
inline IBackendService& backend() {
    static MockBackendService instance;
    return instance;
}

}  // namespace su::app::services
