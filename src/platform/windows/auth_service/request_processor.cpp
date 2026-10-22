#include "logon_secret_server.h"
#include "request_log.h"

#include "client_disconnect_watcher.h"
#include "logon_secret_protocol.h"
#include "pipe_response.h"
#include "recognition_agent_protocol.h"
#include "recognition_result.h"
#include "sid_rate_limiter.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "server_helpers.h"

namespace su::windows::auth_service::detail {
std::string embedding_source(std::span<const float> feature) {
    auto stream = std::ostringstream{};
    stream << "embedding:" << std::setprecision(9);
    for (std::size_t index = 0; index < feature.size(); ++index) {
        if (index != 0) {
            stream << ',';
        }
        stream << feature[index];
    }
    return std::move(stream).str();
}

struct RequestProcessor {
    HANDLE stop_event;
    HANDLE client_gone;
    const Request& request;
    Response& response;
    security::LogonSecretStore& store;
    security::FaceProfileStore& profile_store;
    ManagementAuthorizer& management_authorizer;
    std::mutex& state_mutex;
    std::span<const std::uint8_t> caller_sid;
    DWORD caller_process_id;
    RequestLog& trace;
    std::optional<std::wstring_view> requested_sid;
    std::expected<std::string, Status> sid_utf8 = std::unexpected(Status::kInvalidRequest);

    std::expected<void, Status> validate() {
        if (request.magic != smile2unlock::logon_secret_ipc::kMagic ||
            request.version != smile2unlock::logon_secret_ipc::kVersion ||
            request.request_id == 0) {
            return std::unexpected(Status::kInvalidRequest);
        }
        requested_sid = fixed_wide(request.sid);
        if (!requested_sid) {
            return std::unexpected(Status::kInvalidRequest);
        }
        const auto caller_is_system = is_local_system(caller_sid);
        if ((request.operation == Operation::kPrepare ||
             request.operation == Operation::kAuthenticateAndPrepare ||
             request.operation == Operation::kMarkStale) &&
            !caller_is_system) {
            return std::unexpected(Status::kAccessDenied);
        }
        if (request.operation == Operation::kStore || request.operation == Operation::kClear ||
            request.operation == Operation::kEnrollProfile ||
            request.operation == Operation::kListProfiles ||
            request.operation == Operation::kDeleteProfile ||
            request.operation == Operation::kVerifyProfile ||
            request.operation == Operation::kCredentialStatus ||
            request.operation == Operation::kSetRecognitionSettings) {
            const auto caller_sid_text = sid_string(caller_sid);
            if (!caller_sid_text || !std::ranges::equal(*caller_sid_text, *requested_sid)) {
                return std::unexpected(Status::kAccessDenied);
            }
        }
        sid_utf8 = wide_to_utf8(*requested_sid);
        if (!sid_utf8) {
            return std::unexpected(sid_utf8.error());
        }

        return {};
    }

    Status prepare() {
        // Version 2 closes the old bypass: password release is only reachable
        // through kAuthenticateAndPrepare after service-owned face matching.
        return Status::kAccessDenied;
    }

    Status mark_stale() {
        const auto lock = std::lock_guard{state_mutex};
        const auto marked = store.mark_stale(*sid_utf8);
        return marked ? Status::kOk : (trace.cause(marked.error()), map_error(marked.error()));
    }

    Status handle_store() {
        const auto password = fixed_wide(request.password);
        const auto account = resolve_account(caller_sid, trace);
        if (!password || !account) {
            return account ? Status::kInvalidRequest : account.error();
        }
        if (password->empty()) {
            return Status::kInvalidRequest;
        }
        if (!verify_windows_password(*account, *password, trace)) {
            return Status::kAuthenticationFailed;
        }
        const auto stored = [&] {
            const auto lock = std::lock_guard{state_mutex};
            return store.store(*sid_utf8, account->canonical_username, account->kind,
                               std::span<const wchar_t>{password->data(), password->size()});
        }();
        if (!stored) {
            return (trace.cause(stored.error()), map_error(stored.error()));
        }
        if (!management_authorizer.issue(*requested_sid, caller_process_id, response.password,
                                         std::size(response.password))) {
            trace.causes.push_back(log::field("cause", "management_grant_issue_failed"));
            return Status::kUnavailable;
        }
        response.password_length = ManagementAuthorizer::kTokenCharacters;
        return Status::kOk;
    }

    Status clear() {
        const auto password = fixed_wide(request.password);
        const auto account = resolve_account(caller_sid, trace);
        if (!password || !account) {
            return account ? Status::kInvalidRequest : account.error();
        }
        if (!verify_windows_password(*account, *password, trace)) {
            return Status::kAuthenticationFailed;
        }
        const auto cleared = [&] {
            const auto lock = std::lock_guard{state_mutex};
            return store.clear(*sid_utf8);
        }();
        return cleared ? Status::kOk : (trace.cause(cleared.error()), map_error(cleared.error()));
    }

    Status authenticate_and_prepare() {
        const auto settings = recognition_settings(*requested_sid);
        // The agent run is the long pole (up to ~33 s) and touches no shared
        // state: it must stay outside state_mutex so store and profile
        // mutations from other workers proceed during a recognition attempt.
        // client_gone terminates the agent as soon as the pipe client
        // disconnects (credential-provider cancel or deadline).
        const auto feature = run_recognition_agent(stop_event, client_gone,
                                                   request.logon_session_id, settings, trace);
        if (!feature) {
            trace.cause(feature.error());
            return recognition_error_status(feature.error());
        }
        const auto source = embedding_source(*feature);
        const auto lock = std::lock_guard{state_mutex};
        const auto report =
            profile_store.authenticate(*sid_utf8, source, settings.recognition_threshold, true);
        if (!report) {
            return (trace.cause(report.error()), map_profile_error(report.error()));
        }
        if (!report->accepted || !report->liveness_ok) {
            return Status::kAuthenticationFailed;
        }
        auto password = store.prepare(*sid_utf8, request.request_id, request.logon_session_id);
        if (!password || password->size() >= std::size(response.password)) {
            return password ? Status::kCorrupt
                            : (trace.cause(password.error()), map_error(password.error()));
        }
        std::copy_n(password->c_str(), password->size() + 1, response.password);
        response.password_length = static_cast<std::uint32_t>(password->size());
        return Status::kOk;
    }

    Status enroll_profile() {
        const auto label = fixed_wide(request.canonical_username);
        const auto management_token = fixed_wide(request.password);
        const auto payload = request_payload(request);
        if (!label || !management_token || !payload) {
            return Status::kInvalidRequest;
        }
        if (!management_authorizer.consume(*requested_sid, caller_process_id, *management_token)) {
            return Status::kAccessDenied;
        }
        const auto label_utf8 = wide_to_utf8(*label);
        if (!label_utf8) {
            return label_utf8.error();
        }
        const auto credential_ready = [&] {
            const auto lock = std::lock_guard{state_mutex};
            return store.configured(*sid_utf8);
        }();
        if (!credential_ready || !*credential_ready) {
            if (credential_ready)
                trace.causes.push_back(log::field("cause", "stored_credential_missing"));
            return credential_ready ? Status::kUnavailable
                                    : (trace.cause(credential_ready.error()),
                                       map_error(credential_ready.error()));
        }
        const auto enrolled = [&] {
            const auto lock = std::lock_guard{state_mutex};
            return profile_store.enroll(*sid_utf8, *label_utf8, *payload);
        }();
        if (!enrolled) {
            return (trace.cause(enrolled.error()), map_profile_error(enrolled.error()));
        }
        if (const auto listed = list_profiles(); listed != Status::kOk)
            return listed;
        return issue_management_grant();
    }

    Status issue_management_grant() {
        if (!management_authorizer.issue(*requested_sid, caller_process_id, response.password,
                                         std::size(response.password))) {
            trace.causes.push_back(log::field("cause", "management_grant_issue_failed"));
            return Status::kUnavailable;
        }
        response.password_length = ManagementAuthorizer::kTokenCharacters;
        return Status::kOk;
    }

    Status list_profiles() {
        const auto profiles = [&] {
            const auto lock = std::lock_guard{state_mutex};
            return profile_store.list_json(*sid_utf8);
        }();
        return profiles && copy_payload(*profiles, response)
                   ? Status::kOk
                   : (profiles
                          ? Status::kCorrupt
                          : (trace.cause(profiles.error()), map_profile_error(profiles.error())));
    }

    Status delete_profile() {
        const auto management_token = fixed_wide(request.password);
        const auto profile_id = request_payload(request);
        if (!management_token || !profile_id) {
            return Status::kInvalidRequest;
        }
        if (!management_authorizer.consume(*requested_sid, caller_process_id, *management_token)) {
            return Status::kAccessDenied;
        }
        const auto removed = [&] {
            const auto lock = std::lock_guard{state_mutex};
            return profile_store.remove(*sid_utf8, *profile_id);
        }();
        if (!removed) {
            return (trace.cause(removed.error()), map_profile_error(removed.error()));
        }
        if (!*removed) {
            return Status::kProfileNotFound;
        }
        if (!management_authorizer.issue(*requested_sid, caller_process_id, response.password,
                                         std::size(response.password))) {
            trace.causes.push_back(log::field("cause", "management_grant_issue_failed"));
            return Status::kUnavailable;
        }
        response.password_length = ManagementAuthorizer::kTokenCharacters;
        return Status::kOk;
    }

    Status verify_profile() {
        const auto payload = request_payload(request);
        if (!payload) {
            return Status::kInvalidRequest;
        }
        const auto report = [&] {
            const auto lock = std::lock_guard{state_mutex};
            return profile_store.authenticate(
                *sid_utf8, *payload, recognition_settings(*requested_sid).recognition_threshold,
                request.account_kind == 1);
        }();
        if (!report) {
            return (trace.cause(report.error()), map_profile_error(report.error()));
        }
        const auto report_json = std::format(
            R"({{"accepted":{},"score":{},"threshold":{},"liveness_ok":{},"profile_count":{},"best_profile_id":"{}","best_profile_label":"{}","reason":"{}"}})",
            report->accepted ? "true" : "false", report->score, report->threshold,
            report->liveness_ok ? "true" : "false", report->profile_count,
            json_escape(fixed_utf8(report->best_profile_id, std::size(report->best_profile_id))),
            json_escape(
                fixed_utf8(report->best_profile_label, std::size(report->best_profile_label))),
            json_escape(fixed_utf8(report->reason, std::size(report->reason))));
        return copy_payload(report_json, response) ? Status::kOk : Status::kCorrupt;
    }

    Status credential_status() {
        const auto configured = [&] {
            const auto lock = std::lock_guard{state_mutex};
            return store.configured(*sid_utf8);
        }();
        if (!configured) {
            return (trace.cause(configured.error()), map_error(configured.error()));
        }
        response.payload[0] = *configured ? 1 : 0;
        response.payload_length = 1;
        return Status::kOk;
    }

    Status set_recognition_settings() {
        if (request.payload_length != sizeof(RecognitionSettingsPayload)) {
            return Status::kInvalidRequest;
        }
        auto payload = RecognitionSettingsPayload{};
        std::memcpy(&payload, request.payload, sizeof(payload));
        if (!valid_recognition_settings(payload)) {
            return Status::kInvalidRequest;
        }
        return write_recognition_settings(*requested_sid, payload, trace) ? Status::kOk
                                                                          : Status::kUnavailable;
    }

    Status run() {
        const auto valid = validate();
        if (!valid)
            return valid.error();
        switch (request.operation) {
        case Operation::kPrepare:
            return prepare();
        case Operation::kMarkStale:
            return mark_stale();
        case Operation::kStore:
            return handle_store();
        case Operation::kClear:
            return clear();
        case Operation::kAuthenticateAndPrepare:
            return authenticate_and_prepare();
        case Operation::kEnrollProfile:
            return enroll_profile();
        case Operation::kListProfiles:
            return list_profiles();
        case Operation::kDeleteProfile:
            return delete_profile();
        case Operation::kVerifyProfile:
            return verify_profile();
        case Operation::kCredentialStatus:
            return credential_status();
        case Operation::kSetRecognitionSettings:
            return set_recognition_settings();
        }
        return Status::kInvalidRequest;
    }
};

Status process_request(HANDLE stop_event, HANDLE client_gone, const Request& request,
                       Response& response, security::LogonSecretStore& store,
                       security::FaceProfileStore& profile_store,
                       ManagementAuthorizer& management_authorizer, std::mutex& state_mutex,
                       std::span<const std::uint8_t> caller_sid, DWORD caller_process_id,
                       RequestLog& trace) {
    return RequestProcessor{
        stop_event,    client_gone,           request,     response,   store,
        profile_store, management_authorizer, state_mutex, caller_sid, caller_process_id,
        trace,         std::nullopt}
        .run();
}
} // namespace su::windows::auth_service::detail
