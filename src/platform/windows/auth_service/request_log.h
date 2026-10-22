#pragma once
#include "common/log/core.h"
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
    case kListCaptures: return "list_captures";
    case kFetchCapture: return "fetch_capture";
    case kDeleteCapture: return "delete_capture";
    case kDeleteAllCaptures: return "delete_all_captures";
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
    case kNoFaceDetected:
        return "no_face_detected";
    case kProfileNotFound:
        return "profile_not_found";
    }
    return "unknown";
}
inline std::vector<log::Field> completion_fields(
    std::uint64_t id, smile2unlock::logon_secret_ipc::Operation operation,
    smile2unlock::logon_secret_ipc::Status status, std::int64_t duration_ms, std::string_view sid) {
    return {log::field("request_id", id), log::field("op", operation_name(operation)),
            log::field("status", status_name(status)), log::field("duration_ms", duration_ms),
            log::field("sid", sid)};
}
} // namespace su::windows::auth_service
