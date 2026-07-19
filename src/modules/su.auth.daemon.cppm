module;

#include <pwd.h>
#include <cerrno>
#include <cstdio>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <unistd.h>

export module su.auth.daemon;

import std;
import su.control.socket;
import su.core.types;
import su.recognizer.service;

export namespace su::auth {

int run_daemon(std::string_view socket_path = su::control::kDefaultSocketPath);

} // namespace su::auth

namespace su::auth {

namespace {

// DMS abandons a stalled unlock request after eight seconds. Leave enough
// time for the face result to return and its password stack to take over.
constexpr auto kAuthenticationTimeout = std::chrono::seconds{6};
constexpr auto kRetryInterval = std::chrono::milliseconds{80};
constexpr auto kCameraAcquireTimeout = std::chrono::milliseconds{1200};

struct UserPaths {
    std::uint32_t uid = 0;
    std::filesystem::path config;
    std::filesystem::path profiles;
};

struct AttemptResult {
    su::control::ControlResult result = su::control::ControlResult::kUnavailable;
    std::string reason;
};

struct UserAuthData {
    su::app::CoreConfig config;
    std::size_t profile_count = 0;
};

std::string_view message_type_name(su::app::ControlMessageType type) {
    switch (type) {
    case su::app::ControlMessageType::kAuthenticate: return "authenticate";
    case su::app::ControlMessageType::kStatus: return "status";
    case su::app::ControlMessageType::kCancel: return "cancel";
    }
    std::unreachable();
}

std::string log_value(std::string_view value, std::size_t limit = 160) {
    auto output = std::string{};
    output.reserve(std::min(value.size(), limit));
    for (const auto character : value.substr(0, limit)) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            output += static_cast<unsigned char>(character) < 0x20 ? '?' : character;
        }
    }
    return output;
}

std::expected<UserPaths, std::string> paths_for_user(std::string_view username) {
    auto buffer_size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (buffer_size < 1024) {
        buffer_size = 16 * 1024;
    }
    auto buffer = std::vector<char>(static_cast<std::size_t>(buffer_size));
    auto entry = passwd{};
    auto* result = static_cast<passwd*>(nullptr);
    const auto owned_username = std::string(username);
    if (::getpwnam_r(
            owned_username.c_str(),
            &entry,
            buffer.data(),
            buffer.size(),
            &result) != 0
        || result == nullptr
        || entry.pw_dir == nullptr
        || entry.pw_dir[0] == '\0') {
        return std::unexpected("unknown PAM user");
    }

    const auto home = std::filesystem::path(entry.pw_dir);
    return UserPaths{
        .uid = static_cast<std::uint32_t>(entry.pw_uid),
        .config = home / ".config" / "smile2unlock" / "config.toml",
        .profiles = home / ".local" / "share" / "smile2unlock" / "profiles.json",
    };
}

std::optional<std::string_view> peer_authorization_error(
    std::uint32_t peer_uid,
    const su::app::ControlRequest& request) {
    if (peer_uid == 0) {
        return std::nullopt;
    }
    if (request.type != su::app::ControlMessageType::kAuthenticate) {
        return "user peer may only authenticate";
    }
    const auto paths = paths_for_user(request.username);
    if (!paths || paths->uid != peer_uid) {
        return "peer identity does not match requested user";
    }
    return std::nullopt;
}

bool open_camera_with_retry(
    su::recognizer::RecognizerService& recognizer,
    int camera_index,
    std::chrono::steady_clock::time_point deadline) {
    while (true) {
        if (recognizer.open_camera(camera_index)) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        std::this_thread::sleep_until(std::min(deadline, now + kRetryInterval));
    }
}

bool secure_user_file(const std::filesystem::path& path, std::uint32_t uid, bool required) {
    struct stat metadata {};
    if (::lstat(path.c_str(), &metadata) != 0) {
        return !required && errno == ENOENT;
    }
    return S_ISREG(metadata.st_mode)
        && static_cast<std::uint32_t>(metadata.st_uid) == uid
        && (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

class CameraGuard {
public:
    explicit CameraGuard(su::recognizer::RecognizerService& recognizer)
        : recognizer_(recognizer) {}
    ~CameraGuard() { recognizer_.close_camera(); }
    CameraGuard(const CameraGuard&) = delete;
    CameraGuard& operator=(const CameraGuard&) = delete;

private:
    su::recognizer::RecognizerService& recognizer_;
};

class ActiveRequestGuard {
public:
    ActiveRequestGuard(
        std::atomic<std::uint64_t>& active_request,
        std::atomic<bool>& cancel_requested,
        std::uint64_t request_id)
        : active_request_(active_request), cancel_requested_(cancel_requested) {
        active_request_.store(request_id, std::memory_order_release);
        cancel_requested_.store(false, std::memory_order_release);
    }

    ~ActiveRequestGuard() {
        active_request_.store(0, std::memory_order_release);
        cancel_requested_.store(false, std::memory_order_release);
    }

    ActiveRequestGuard(const ActiveRequestGuard&) = delete;
    ActiveRequestGuard& operator=(const ActiveRequestGuard&) = delete;

private:
    std::atomic<std::uint64_t>& active_request_;
    std::atomic<bool>& cancel_requested_;
};

class FsUidGuard {
public:
    explicit FsUidGuard(std::uint32_t uid)
        : previous_(::setfsuid(static_cast<uid_t>(uid))) {
        valid_ = static_cast<uid_t>(::setfsuid(static_cast<uid_t>(-1)))
            == static_cast<uid_t>(uid);
    }

    ~FsUidGuard() {
        (void)::setfsuid(previous_);
    }

    FsUidGuard(const FsUidGuard&) = delete;
    FsUidGuard& operator=(const FsUidGuard&) = delete;
    [[nodiscard]] bool valid() const { return valid_; }

private:
    uid_t previous_ = 0;
    bool valid_ = false;
};

std::expected<UserAuthData, std::string> load_user_auth_data(const UserPaths& paths) {
    // setfsuid is per-thread on Linux. All user-controlled path traversal and
    // file reads run with the target user's filesystem permissions, then the
    // daemon restores root before touching the camera device.
    const auto fsuid = FsUidGuard{paths.uid};
    if (!fsuid.valid()) {
        return std::unexpected("failed to enter user filesystem context");
    }
    const auto config = su::app::load_config(paths.config.string());
    if (!config) {
        return std::unexpected("failed to load user config");
    }
    const auto profiles = su::app::list_face_profile_summaries(paths.profiles.string());
    if (!profiles) {
        return std::unexpected("failed to load face profiles");
    }
    return UserAuthData{.config = *config, .profile_count = profiles->size()};
}

std::expected<su::app::FaceAuthReport, std::string> authenticate_user_sample(
    const UserPaths& paths,
    std::string_view sample,
    float threshold) {
    const auto fsuid = FsUidGuard{paths.uid};
    if (!fsuid.valid()) {
        return std::unexpected("failed to enter user filesystem context");
    }
    const auto report = su::app::authenticate_face_sample_report(
        paths.profiles.string(), sample, threshold, true);
    if (!report) {
        return std::unexpected("face profile comparison failed");
    }
    return *report;
}

class AuthService {
public:
    bool available() const {
        return recognizer_.seetaface_available() && !recognizer_.enumerate_cameras().empty();
    }

    AttemptResult authenticate(std::uint64_t request_id, std::string_view username) {
        auto auth_lock = std::unique_lock(auth_mutex_, std::try_to_lock);
        if (!auth_lock.owns_lock()) {
            return {su::control::ControlResult::kBusy, "another authentication is active"};
        }

        const auto active_request = ActiveRequestGuard{
            active_request_, cancel_requested_, request_id};
        const auto deadline = std::chrono::steady_clock::now() + kAuthenticationTimeout;

        const auto paths = paths_for_user(username);
        if (!paths) {
            return {su::control::ControlResult::kRejected, paths.error()};
        }
        if (!secure_user_file(paths->config, paths->uid, false)
            || !secure_user_file(paths->profiles, paths->uid, true)) {
            return {su::control::ControlResult::kUnavailable, "unsafe or missing user data"};
        }

        const auto user_data = load_user_auth_data(*paths);
        if (!user_data) {
            return {su::control::ControlResult::kUnavailable, user_data.error()};
        }
        if (user_data->profile_count == 0) {
            return {su::control::ControlResult::kRejected, "no enrolled face profiles"};
        }
        const auto& config = user_data->config;
        if (!recognizer_.seetaface_available()) {
            return {su::control::ControlResult::kUnavailable, "face models unavailable"};
        }
        const auto camera_deadline = std::min(
            deadline,
            std::chrono::steady_clock::now() + kCameraAcquireTimeout);
        if (!open_camera_with_retry(recognizer_, config.selected_camera, camera_deadline)) {
            return {su::control::ControlResult::kUnavailable, "camera unavailable"};
        }
        const auto close_camera = CameraGuard{recognizer_};
        if (config.liveness_detection) {
            if (auto reset = recognizer_.reset_liveness(); !reset) {
                return {su::control::ControlResult::kUnavailable, "liveness unavailable"};
            }
        }

        auto saw_face = false;
        auto saw_live_face = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel_requested_.load(std::memory_order_acquire)) {
                return {su::control::ControlResult::kCancelled, "authentication cancelled"};
            }

            const auto capture = recognizer_.extract_features(config.liveness_detection);
            if (!capture || !capture->has_face || capture->feature.empty()) {
                std::this_thread::sleep_for(kRetryInterval);
                continue;
            }
            saw_face = true;

            const auto liveness_ok = !config.liveness_detection
                || capture->liveness_score >= config.liveness_threshold;
            if (!liveness_ok) {
                std::this_thread::sleep_for(kRetryInterval);
                continue;
            }
            saw_live_face = true;

            const auto report = authenticate_user_sample(
                *paths,
                su::recognizer::embedding_sample_source(capture->feature),
                config.recognition_threshold);
            if (!report) {
                return {su::control::ControlResult::kUnavailable, report.error()};
            }
            if (report->accepted) {
                return {su::control::ControlResult::kAccepted, "face matched"};
            }
            std::this_thread::sleep_for(kRetryInterval);
        }

        if (!saw_face) {
            return {su::control::ControlResult::kRejected, "no face detected"};
        }
        if (!saw_live_face) {
            return {su::control::ControlResult::kRejected, "liveness check failed"};
        }
        return {su::control::ControlResult::kRejected, "face did not match"};
    }

    bool cancel(std::uint64_t target_request_id) {
        if (active_request_.load(std::memory_order_acquire) != target_request_id) {
            return false;
        }
        cancel_requested_.store(true, std::memory_order_release);
        return true;
    }

    void handle(su::control::Connection connection) {
        const auto peer_uid = connection.peer_uid();
        if (!peer_uid) {
            std::println(stderr, "su_authd event=peer_rejected");
            return;
        }
        const auto payload = connection.receive_frame();
        if (!payload) {
            std::println(stderr, "su_authd event=request_receive_failed");
            return;
        }
        const auto request = su::app::parse_control_request(*payload);
        if (!request) {
            std::println(stderr, "su_authd event=request_invalid");
            return;
        }

        if (const auto reason = peer_authorization_error(*peer_uid, *request)) {
            std::println(
                stderr,
                R"(su_authd event=peer_rejected peer_uid={} request_id={} type={} reason="{}")",
                *peer_uid,
                request->request_id,
                message_type_name(request->type),
                *reason);
            const auto sent = connection.send_frame(su::control::make_response(
                request->request_id,
                su::control::ControlResult::kRejected,
                *reason));
            if (!sent) {
                std::println(
                    stderr,
                    "su_authd event=response_send_failed request_id={}",
                    request->request_id);
            }
            return;
        }

        const auto started_at = std::chrono::steady_clock::now();
        const auto type_name = message_type_name(request->type);
        if (request->type == su::app::ControlMessageType::kAuthenticate) {
            std::println(
                stderr,
                R"(su_authd event=request_started request_id={} type={} user="{}")",
                request->request_id,
                type_name,
                log_value(request->username, 64));
        } else {
            std::println(
                stderr,
                "su_authd event=request_started request_id={} type={}",
                request->request_id,
                type_name);
        }

        auto response = AttemptResult{};
        switch (request->type) {
        case su::app::ControlMessageType::kAuthenticate:
            response = authenticate(request->request_id, request->username);
            break;
        case su::app::ControlMessageType::kStatus:
            response = available()
                ? AttemptResult{su::control::ControlResult::kAccepted, "service available"}
                : AttemptResult{su::control::ControlResult::kUnavailable, "service unavailable"};
            break;
        case su::app::ControlMessageType::kCancel:
            response = cancel(request->target_request_id)
                ? AttemptResult{su::control::ControlResult::kCancelled, "cancel requested"}
                : AttemptResult{su::control::ControlResult::kUnavailable, "request not active"};
            break;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at);
        std::println(
            stderr,
            R"(su_authd event=request_completed request_id={} type={} result={} reason="{}" duration_ms={})",
            request->request_id,
            type_name,
            su::control::control_result_name(response.result),
            log_value(response.reason),
            elapsed.count());

        const auto sent = connection.send_frame(su::control::make_response(
            request->request_id,
            response.result,
            response.reason));
        if (!sent) {
            std::println(
                stderr,
                "su_authd event=response_send_failed request_id={}",
                request->request_id);
        }
    }

private:
    su::recognizer::RecognizerService recognizer_;
    std::mutex auth_mutex_;
    std::atomic<std::uint64_t> active_request_{0};
    std::atomic<bool> cancel_requested_{false};
};

} // namespace

int run_daemon(std::string_view socket_path) {
    if (::geteuid() != 0) {
        std::println(stderr, "su_authd must run as root");
        return 1;
    }

    const auto path = std::filesystem::path(socket_path);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error || ::chmod(path.parent_path().c_str(), 0755) != 0) {
        std::println(stderr, "su_authd failed to prepare runtime directory");
        return 1;
    }

    auto listener = su::control::Listener::bind_to(socket_path);
    if (!listener) {
        std::println(stderr, "su_authd failed to bind {}", socket_path);
        return 1;
    }

    auto service = AuthService{};
    std::println(
        stderr,
        "su_authd event=listening socket={} available={}",
        socket_path,
        service.available());
    while (true) {
        auto connection = listener->accept_one();
        if (!connection) {
            continue;
        }
        // Authentication is intentionally detached from accept so status and
        // cancellation requests remain responsive during camera capture.
        std::thread([&service, client = std::move(*connection)]() mutable {
            service.handle(std::move(client));
        }).detach();
    }
}

} // namespace su::auth
