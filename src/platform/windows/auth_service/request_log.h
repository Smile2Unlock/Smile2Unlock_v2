#pragma once
#include "common/log/logger.h"
#include "logon_secret_protocol.h"
#include "face_profile_store.h"
#include "logon_secret_store.h"
#include "recognition_agent_protocol.h"

namespace su::windows::security {
inline std::vector<log::Field> to_fields(LogonSecretError error) {
    return {log::field("store_error", logon_secret_error_message(error))};
}
inline std::vector<log::Field> to_fields(FaceProfileStoreError error) {
    return {log::field("profile_error", static_cast<int>(error))};
}
} // namespace su::windows::security

namespace smile2unlock::recognition_agent_ipc {
inline std::vector<su::log::Field> to_fields(AgentStatus status) {
    return {su::log::field("agent_status", static_cast<unsigned>(status))};
}
} // namespace smile2unlock::recognition_agent_ipc

namespace su::windows::auth_service {
inline std::string_view operation_name(smile2unlock::logon_secret_ipc::Operation op) noexcept {
    using enum smile2unlock::logon_secret_ipc::Operation;
    switch (op) {
    case kStore:
        return "store";
    case kPrepare:
        return "prepare";
    case kClear:
        return "clear";
    case kMarkStale:
        return "mark_stale";
    case kAuthenticateAndPrepare:
        return "authenticate";
    case kEnrollProfile:
        return "enroll_profile";
    case kListProfiles:
        return "list_profiles";
    case kDeleteProfile:
        return "delete_profile";
    case kVerifyProfile:
        return "verify_profile";
    case kCredentialStatus:
        return "credential_status";
    case kSetRecognitionSettings:
        return "set_recognition_settings";
    }
    return "unknown";
}
inline std::string_view status_name(smile2unlock::logon_secret_ipc::Status status) noexcept {
    using enum smile2unlock::logon_secret_ipc::Status;
    switch (status) {
    case kOk:
        return "ok";
    case kInvalidRequest:
        return "invalid_request";
    case kAccessDenied:
        return "access_denied";
    case kUnavailable:
        return "unavailable";
    case kStaleOrConsumed:
        return "stale_or_consumed";
    case kCorrupt:
        return "corrupt";
    case kAuthenticationFailed:
        return "authentication_failed";
    case kProfileNotFound:
        return "profile_not_found";
    }
    return "unknown";
}
struct RequestLog {
    const log::Logger logger;
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::vector<log::Field> causes = {};
    bool completion_written = false;
    ~RequestLog() {
        // Preserve one completion record when request processing unwinds.
        if (!completion_written)
            completed(smile2unlock::logon_secret_ipc::Status::kUnavailable);
    }
    template <typename E> void cause(const E& error) noexcept {
        try {
            using log::to_fields;
            auto fields = to_fields(error);
            causes.insert(causes.end(), fields.begin(), fields.end());
        } catch (...) {
        }
    }
    void completed(smile2unlock::logon_secret_ipc::Status status) noexcept {
        if (completion_written)
            return;
        completion_written = true;
        try {
            auto fields = causes;
            fields.push_back(log::field("status", status_name(status)));
            fields.push_back(
                log::field("duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - started)
                                              .count()));
            logger.info("request.completed", std::move(fields));
        } catch (...) {
        }
    }
};
} // namespace su::windows::auth_service
