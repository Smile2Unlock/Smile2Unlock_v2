module;

#include <cerrno>
#include <cstdio>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

export module su.auth.daemon;

import std;
import su.control.socket;
import su.core.types;
import su.auth.storage;
import su.auth.user;
import su.recognizer.service;

export namespace su::auth {

int run_daemon(std::string_view socket_path = su::control::kDefaultSocketPath);

} // namespace su::auth

namespace su::auth {

namespace {

constexpr auto kAuthenticationTimeout = std::chrono::seconds{6};
// Liveness needs a sequence of frames; keep its budget below the socket's
// 15-second deadline so PAM still has time to receive the result.
constexpr auto kLivenessAuthenticationTimeout = std::chrono::seconds{12};
constexpr auto kRetryInterval = std::chrono::milliseconds{80};
constexpr auto kCameraAcquireTimeout = std::chrono::milliseconds{1200};
constexpr auto kAuthenticationRateLimit = std::chrono::seconds{1};
constexpr auto kMaxConcurrentConnections = std::ptrdiff_t{16};

struct AttemptResult {
    su::control::ControlResult result = su::control::ControlResult::kUnavailable;
    std::string reason;
    std::string payload_json = "null";
};

struct UserAuthData {
    std::uint32_t uid = 0;
    su::app::CoreConfig config;
    std::size_t profile_count = 0;
    std::string profiles_path;
};

std::string_view message_type_name(su::app::ControlMessageType type) {
    switch (type) {
    case su::app::ControlMessageType::kAuthenticate: return "authenticate";
    case su::app::ControlMessageType::kStatus: return "status";
    case su::app::ControlMessageType::kCancel: return "cancel";
    case su::app::ControlMessageType::kStorageStatus: return "storage_status";
    case su::app::ControlMessageType::kListProfiles: return "list_profiles";
    case su::app::ControlMessageType::kEnrollProfile: return "enroll_profile";
    case su::app::ControlMessageType::kDeleteProfile: return "delete_profile";
    case su::app::ControlMessageType::kMigrateProfiles: return "migrate_profiles";
    case su::app::ControlMessageType::kVerifyProfile: return "verify_profile";
    case su::app::ControlMessageType::kIssueManagementCapability:
        return "issue_management_capability";
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

std::optional<std::string_view> peer_authorization_error(
    std::uint32_t peer_uid,
    const su::app::ControlRequest& request) {
    if (peer_uid == 0) {
        return std::nullopt;
    }
    if (request.type == su::app::ControlMessageType::kIssueManagementCapability) {
        return "only the privileged deployment helper may issue management capabilities";
    }
    if (request.type == su::app::ControlMessageType::kStatus
        || request.type == su::app::ControlMessageType::kStorageStatus) {
        return std::nullopt;
    }
    if (request.type == su::app::ControlMessageType::kCancel) {
        return "user peer may not cancel another process request";
    }
    const auto paths = su::auth::paths_for_username(request.username);
    if (!paths || !su::auth::peer_request_allowed(peer_uid, request.type, paths->uid)) {
        return "peer identity does not match requested user";
    }
    return std::nullopt;
}

std::optional<std::uint32_t> process_session(
    std::uint32_t pid,
    std::optional<std::uint32_t> expected_uid = std::nullopt) {
    if (pid == 0 || !std::in_range<pid_t>(pid)) {
        return std::nullopt;
    }
    if (expected_uid) {
        struct stat status {};
        const auto proc_path = std::filesystem::path{"/proc"} / std::to_string(pid);
        if (::stat(proc_path.c_str(), &status) != 0
            || status.st_uid != static_cast<uid_t>(*expected_uid)) {
            return std::nullopt;
        }
    }
    const auto session = ::getsid(static_cast<pid_t>(pid));
    return session > 0
        ? std::optional{static_cast<std::uint32_t>(session)}
        : std::nullopt;
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

std::expected<UserAuthData, std::string> load_user_auth_data(
    const UserPaths& paths,
    const MasterKey& master_key) {
    // setfsuid is per-thread. Restrict only the user-owned config read; the
    // encrypted profile store remains root-owned and is opened after restore.
    const auto config = [&paths]() -> std::expected<su::app::CoreConfig, std::string> {
        const auto fsuid = FsUidGuard{paths.uid};
        if (!fsuid.valid()) {
            return std::unexpected("failed to enter user filesystem context");
        }
        const auto config_file = su::auth::open_user_file(
            paths, su::auth::UserFileKind::kConfig, false);
        if (!config_file) {
            return std::unexpected(config_file.error());
        }
        const auto loaded = config_file->has_value()
            ? su::app::load_config(config_file->value().proc_path())
            : std::expected<su::app::CoreConfig, su::app::CoreError>{su::app::default_config()};
        if (!loaded) {
            return std::unexpected("failed to load user config");
        }
        return *loaded;
    }();
    if (!config) {
        return std::unexpected(config.error());
    }
    const auto profiles = master_key.with_context(
        paths.uid, [&](const su::app::EncryptedStoreContext& context) {
            return su::app::list_encrypted_face_profile_summaries(
                context, paths.profiles.string());
        });
    if (!profiles) {
        return std::unexpected("failed to decrypt face profiles");
    }
    return UserAuthData{
        .uid = paths.uid,
        .config = *config,
        .profile_count = profiles->size(),
        .profiles_path = paths.profiles.string(),
    };
}

std::expected<su::app::FaceAuthReport, std::string> authenticate_user_sample(
    const UserAuthData& user_data,
    const MasterKey& master_key,
    std::string_view sample,
    float threshold) {
    const auto report = master_key.with_context(
        user_data.uid, [&](const su::app::EncryptedStoreContext& context) {
            return su::app::authenticate_encrypted_face_sample_report(
                context, user_data.profiles_path, sample, threshold, true);
        });
    if (!report) {
        return std::unexpected("face profile comparison failed");
    }
    return *report;
}

class AuthService {
public:
    explicit AuthService(std::optional<MasterKey> master_key)
        : master_key_(std::move(master_key)) {}

    bool available() const {
        return availability_reason() == "service available";
    }

    std::string availability_reason() const {
        if (!master_key_) {
            return "encrypted storage key unavailable";
        }
        if (!recognizer_.seetaface_available()) {
            return "face recognition models unavailable";
        }
        if (recognizer_.enumerate_cameras().empty()) {
            return "no V4L2 camera detected";
        }
        return "service available";
    }

    std::string_view storage_status() const {
        return master_key_
            ? key_protection_name(master_key_->protection())
            : "unavailable";
    }

    AttemptResult authenticate(std::uint64_t request_id, std::string_view username) {
        auto auth_lock = std::unique_lock(auth_mutex_, std::try_to_lock);
        if (!auth_lock.owns_lock()) {
            return {su::control::ControlResult::kBusy, "another authentication is active"};
        }

        const auto active_request = ActiveRequestGuard{
            active_request_, cancel_requested_, request_id};
        if (!master_key_) {
            return {su::control::ControlResult::kUnavailable, "encrypted storage key unavailable"};
        }

        const auto paths = su::auth::paths_for_username(username);
        if (!paths) {
            const auto result = paths.error() == su::auth::UserLookupError::kUnknown
                ? su::control::ControlResult::kRejected
                : su::control::ControlResult::kUnavailable;
            return {result, std::string(su::auth::user_lookup_error_message(paths.error()))};
        }
        const auto now = std::chrono::steady_clock::now();
        const auto last_started = last_auth_started_.contains(paths->uid)
            ? std::optional{last_auth_started_.at(paths->uid)}
            : std::nullopt;
        if (su::auth::authentication_rate_limited(
                last_started, now, kAuthenticationRateLimit)) {
            return {su::control::ControlResult::kBusy, "authentication rate limited"};
        }
        last_auth_started_[paths->uid] = now;
        const auto user_data = load_user_auth_data(*paths, *master_key_);
        if (!user_data) {
            return {su::control::ControlResult::kUnavailable, user_data.error()};
        }
        if (user_data->profile_count == 0) {
            return {su::control::ControlResult::kRejected, "no enrolled face profiles"};
        }
        const auto& config = user_data->config;
        const auto authentication_timeout = config.liveness_detection
            ? kLivenessAuthenticationTimeout
            : kAuthenticationTimeout;
        const auto deadline = std::chrono::steady_clock::now() + authentication_timeout;
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
                *user_data,
                *master_key_,
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

    AttemptResult list_profiles(std::string_view username) const {
        const auto paths = paths_for_request(username);
        if (!paths) {
            return {su::control::ControlResult::kRejected, paths.error()};
        }
        if (!master_key_) {
            return {su::control::ControlResult::kUnavailable, "encrypted storage key unavailable"};
        }
        const auto profiles = master_key_->with_context(
            paths->uid, [&](const su::app::EncryptedStoreContext& context) {
                return su::app::list_encrypted_face_profiles_json(
                    context, paths->profiles.string());
            });
        return profiles
            ? AttemptResult{su::control::ControlResult::kAccepted, "profiles listed", *profiles}
            : AttemptResult{su::control::ControlResult::kUnavailable, "failed to decrypt profiles"};
    }

    AttemptResult enroll_profile(
        std::string_view username,
        std::string_view label,
        std::string_view sample) const {
        const auto paths = paths_for_request(username);
        if (!paths) {
            return {su::control::ControlResult::kRejected, paths.error()};
        }
        if (!master_key_) {
            return {su::control::ControlResult::kUnavailable, "encrypted storage key unavailable"};
        }
        const auto enrolled = master_key_->with_context(
            paths->uid, [&](const su::app::EncryptedStoreContext& context) {
                return su::app::enroll_encrypted_face_profile(
                    context, paths->profiles.string(), label, sample);
            });
        if (!enrolled) {
            return {su::control::ControlResult::kUnavailable, "failed to encrypt face profile"};
        }
        return list_profiles(username);
    }

    AttemptResult delete_profile(
        std::string_view username,
        std::string_view profile_id) const {
        const auto paths = paths_for_request(username);
        if (!paths) {
            return {su::control::ControlResult::kRejected, paths.error()};
        }
        if (!master_key_) {
            return {su::control::ControlResult::kUnavailable, "encrypted storage key unavailable"};
        }
        const auto deleted = master_key_->with_context(
            paths->uid, [&](const su::app::EncryptedStoreContext& context) {
                return su::app::delete_encrypted_face_profile(
                    context, paths->profiles.string(), profile_id);
            });
        if (!deleted) {
            return {su::control::ControlResult::kUnavailable, "failed to update encrypted profiles"};
        }
        if (!*deleted) {
            return {su::control::ControlResult::kRejected, "profile not found"};
        }
        return list_profiles(username);
    }

    AttemptResult verify_profile(
        std::string_view username,
        std::string_view sample,
        bool liveness_ok) const {
        const auto paths = paths_for_request(username);
        if (!paths) {
            return {su::control::ControlResult::kRejected, paths.error()};
        }
        if (!master_key_) {
            return {su::control::ControlResult::kUnavailable, "encrypted storage key unavailable"};
        }
        const auto user_data = load_user_auth_data(*paths, *master_key_);
        if (!user_data) {
            return {su::control::ControlResult::kUnavailable, user_data.error()};
        }
        const auto report = master_key_->with_context(
            user_data->uid, [&](const su::app::EncryptedStoreContext& context) {
                return su::app::authenticate_encrypted_face_sample_report(
                    context, user_data->profiles_path, sample,
                    user_data->config.recognition_threshold, liveness_ok);
            });
        if (!report) {
            return {su::control::ControlResult::kUnavailable, "face profile comparison failed"};
        }
        const auto payload = nlohmann::json{
            {"accepted", report->accepted},
            {"score", report->score},
            {"threshold", report->threshold},
            {"liveness_ok", report->liveness_ok},
            {"profile_count", report->profile_count},
            {"best_profile_id", report->best_profile_id},
            {"best_profile_label", report->best_profile_label},
            {"reason", report->reason},
        }.dump();
        return {
            report->accepted
                ? su::control::ControlResult::kAccepted
                : su::control::ControlResult::kRejected,
            report->accepted ? "face matched" : "face did not match",
            payload,
        };
    }

    AttemptResult migrate_profiles(std::string_view username) const {
        const auto paths = paths_for_request(username);
        if (!paths) {
            return {su::control::ControlResult::kRejected, paths.error()};
        }
        if (!master_key_) {
            return {su::control::ControlResult::kUnavailable, "encrypted storage key unavailable"};
        }

        auto legacy = std::optional<PinnedUserFile>{};
        {
            const auto fsuid = FsUidGuard{paths->uid};
            if (!fsuid.valid()) {
                return {su::control::ControlResult::kUnavailable, "failed to enter user filesystem context"};
            }
            auto opened = open_user_file(*paths, UserFileKind::kProfiles, false);
            if (!opened) {
                return {su::control::ControlResult::kUnavailable, opened.error()};
            }
            if (!opened->has_value()) {
                return list_profiles(username);
            }
            legacy.emplace(std::move(opened->value()));
        }

        const auto migrated = master_key_->with_context(
            paths->uid, [&](const su::app::EncryptedStoreContext& context) {
                return su::app::migrate_plaintext_face_profiles(
                    context, legacy->proc_path(), paths->profiles.string());
            });
        if (!migrated) {
            return {su::control::ControlResult::kUnavailable, "profile migration failed"};
        }
        if (*migrated) {
            const auto fsuid = FsUidGuard{paths->uid};
            if (!fsuid.valid()) {
                return {su::control::ControlResult::kUnavailable, "failed to restore user filesystem context"};
            }
            if (const auto removed = remove_pinned_user_file(
                    *paths, UserFileKind::kProfiles, *legacy); !removed) {
                return {su::control::ControlResult::kUnavailable, removed.error()};
            }
        }
        auto listed = list_profiles(username);
        listed.reason = *migrated
            ? "legacy profiles migrated"
            : "encrypted profile store already exists; legacy profile retained";
        return listed;
    }

    AttemptResult issue_management_capability(
        std::uint32_t target_uid,
        std::uint32_t target_pid,
        std::string_view operation_name) {
        const auto operation = management_operation_from_name(operation_name);
        const auto session = process_session(target_pid, target_uid);
        if (!operation || !session) {
            return {su::control::ControlResult::kRejected, "management capability target is invalid"};
        }
        auto token = management_capabilities_.issue(
            target_uid, target_pid, *session, *operation);
        if (!token) {
            return {su::control::ControlResult::kUnavailable, token.error()};
        }
        auto payload = nlohmann::json{{"token", *token}}.dump();
        std::ranges::fill(*token, '\0');
        token->clear();
        return {
            su::control::ControlResult::kAccepted,
            "management capability issued",
            std::move(payload),
        };
    }

    bool consume_management_capability(
        su::app::ControlRequest& request,
        const su::control::PeerCredentials& peer) {
        const auto operation = management_operation_for_message(request.type);
        const auto session = process_session(peer.pid, peer.uid);
        const auto allowed = operation && session
            && management_capabilities_.consume(
                request.management_token,
                peer.uid,
                peer.pid,
                *session,
                *operation);
        std::ranges::fill(request.management_token, '\0');
        request.management_token.clear();
        return allowed;
    }

    bool cancel(std::uint64_t target_request_id) {
        if (active_request_.load(std::memory_order_acquire) != target_request_id) {
            return false;
        }
        cancel_requested_.store(true, std::memory_order_release);
        return true;
    }

    void handle(su::control::Connection connection) {
        const auto peer = connection.peer_credentials();
        if (!peer) {
            std::println(stderr, "su_authd event=peer_rejected");
            return;
        }
        auto payload = connection.receive_frame();
        if (!payload) {
            std::println(stderr, "su_authd event=request_receive_failed");
            return;
        }
        auto request = su::app::parse_control_request(*payload);
        std::ranges::fill(*payload, '\0');
        if (!request) {
            std::println(stderr, "su_authd event=request_invalid");
            return;
        }

        if (const auto reason = peer_authorization_error(peer->uid, *request)) {
            std::println(
                stderr,
                R"(su_authd event=peer_rejected peer_uid={} request_id={} type={} reason="{}")",
                peer->uid,
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

        if (management_operation_for_message(request->type)
            && !consume_management_capability(*request, *peer)) {
            const auto sent = connection.send_frame(su::control::make_response(
                request->request_id,
                su::control::ControlResult::kRejected,
                "profile management authorization is missing, expired, or invalid"));
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
        case su::app::ControlMessageType::kStatus: {
            const auto reason = availability_reason();
            response = reason == "service available"
                ? AttemptResult{su::control::ControlResult::kAccepted, reason}
                : AttemptResult{su::control::ControlResult::kUnavailable, reason};
            break;
        }
        case su::app::ControlMessageType::kCancel:
            response = cancel(request->target_request_id)
                ? AttemptResult{su::control::ControlResult::kCancelled, "cancel requested"}
                : AttemptResult{su::control::ControlResult::kUnavailable, "request not active"};
            break;
        case su::app::ControlMessageType::kStorageStatus:
            response = master_key_
                ? AttemptResult{
                    su::control::ControlResult::kAccepted,
                    "encrypted storage available",
                    nlohmann::json{{"protection", storage_status()}}.dump()}
                : AttemptResult{
                    su::control::ControlResult::kUnavailable,
                    "encrypted storage key unavailable",
                    nlohmann::json{{"protection", "unavailable"}}.dump()};
            break;
        case su::app::ControlMessageType::kListProfiles:
            response = list_profiles(request->username);
            break;
        case su::app::ControlMessageType::kEnrollProfile:
            response = enroll_profile(
                request->username, request->label, request->face_sample_source);
            break;
        case su::app::ControlMessageType::kDeleteProfile:
            response = delete_profile(request->username, request->profile_id);
            break;
        case su::app::ControlMessageType::kMigrateProfiles:
            response = migrate_profiles(request->username);
            break;
        case su::app::ControlMessageType::kVerifyProfile:
            response = verify_profile(
                request->username, request->face_sample_source, request->liveness_ok);
            break;
        case su::app::ControlMessageType::kIssueManagementCapability:
            response = issue_management_capability(
                request->target_uid, request->target_pid, request->management_operation);
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

        auto response_frame = su::control::make_response(
            request->request_id,
            response.result,
            response.reason,
            response.payload_json);
        const auto sent = connection.send_frame(response_frame);
        std::ranges::fill(response_frame, '\0');
        std::ranges::fill(response.payload_json, '\0');
        response.payload_json.clear();
        if (!sent) {
            std::println(
                stderr,
                "su_authd event=response_send_failed request_id={}",
                request->request_id);
        }
    }

private:
    static std::expected<UserPaths, std::string> paths_for_request(std::string_view username) {
        const auto paths = paths_for_username(username);
        if (!paths) {
            return std::unexpected(std::string(user_lookup_error_message(paths.error())));
        }
        return *paths;
    }

    std::optional<MasterKey> master_key_;
    su::recognizer::RecognizerService recognizer_;
    std::mutex auth_mutex_;
    std::atomic<std::uint64_t> active_request_{0};
    std::atomic<bool> cancel_requested_{false};
    ManagementCapabilityStore management_capabilities_;
    std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point>
        last_auth_started_;
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

    auto loaded_key = load_systemd_key_credential();
    if (!loaded_key) {
        std::println(
            stderr,
            "su_authd event=storage_unavailable reason={}",
            key_provider_error_message(loaded_key.error()));
    }
    auto service = AuthService{
        loaded_key
            ? std::optional<MasterKey>{std::move(*loaded_key)}
            : std::nullopt};
    std::println(
        stderr,
        "su_authd event=listening socket={} available={} storage={}",
        socket_path,
        service.available(),
        service.storage_status());
    auto connection_slots = std::counting_semaphore<kMaxConcurrentConnections>{
        kMaxConcurrentConnections};
    while (true) {
        auto connection = listener->accept_one();
        if (!connection) {
            continue;
        }
        if (!connection_slots.try_acquire()) {
            std::println(stderr, "su_authd event=connection_rejected reason=capacity");
            continue;
        }
        // Authentication is intentionally detached from accept so status and
        // cancellation requests remain responsive during camera capture. The
        // semaphore bounds detached worker lifetime and memory consumption.
        std::thread([&service, &connection_slots, client = std::move(*connection)]() mutable {
            struct SlotRelease {
                std::counting_semaphore<kMaxConcurrentConnections>& slots;
                ~SlotRelease() { slots.release(); }
            };
            const auto release_slot = SlotRelease{connection_slots};
            service.handle(std::move(client));
        }).detach();
    }
}

} // namespace su::auth
