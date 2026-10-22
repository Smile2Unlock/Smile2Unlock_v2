#pragma once
#include "logon_secret_server.h"
#include "request_log.h"
#include "management_authorizer.h"
#include <span>
#include <vector>
#include <optional>
#include <string_view>
#include <algorithm>

namespace su::windows::auth_service::detail {
using smile2unlock::logon_secret_ipc::Operation;
using smile2unlock::logon_secret_ipc::RecognitionSettingsPayload;
using smile2unlock::logon_secret_ipc::Request;
using smile2unlock::logon_secret_ipc::Response;
using smile2unlock::logon_secret_ipc::Status;
struct LocalMemoryDeleter {
    void operator()(void* pointer) const noexcept {
        if (pointer != nullptr) {
            LocalFree(pointer);
        }
    }
};

struct HandleDeleter {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

using LocalMemory = std::unique_ptr<void, LocalMemoryDeleter>;
using ScopedHandle = std::unique_ptr<void, HandleDeleter>;

struct ResolvedAccount {
    std::string canonical_username;
    SuWindowsAccountKind kind;
    std::wstring domain;
    std::wstring username;
};

struct RecognitionSettings {
    std::int32_t camera_index = 0;
    float recognition_threshold = 0.65F;
    bool liveness_detection = true;
    float liveness_threshold = 0.5F;
    // Per-capture agent deadline, from the same TimeoutSec the credential
    // provider uses for its overall attempt.
    std::uint32_t timeout_ms = 10'000;
};

template <std::size_t N> std::optional<std::wstring_view> fixed_wide(const wchar_t (&value)[N]) {
    const auto length = wcsnlen(value, N);
    if (length == 0 || length >= N) {
        return std::nullopt;
    }
    return std::wstring_view{value, length};
}

std::expected<bool, DWORD> wait_for_pipe_client(HANDLE pipe, HANDLE stop_event);
std::expected<bool, DWORD> read_request(HANDLE pipe, HANDLE stop_event, Request& request,
                                        DWORD& bytes_read);
std::expected<LocalMemory, DWORD> pipe_descriptor();
std::expected<std::vector<std::uint8_t>, DWORD> client_sid(HANDLE pipe);
std::expected<DWORD, DWORD> client_process_id(HANDLE pipe);
bool is_local_system(std::span<const std::uint8_t> sid);
std::expected<std::wstring, DWORD> sid_string(std::span<const std::uint8_t> sid);
std::expected<std::string, Status> wide_to_utf8(std::wstring_view value);
RecognitionSettings recognition_settings(std::wstring_view sid);
bool valid_recognition_settings(const RecognitionSettingsPayload& payload);
bool write_recognition_settings(std::wstring_view sid, const RecognitionSettingsPayload& payload,
                                RequestLog& trace);
std::expected<ResolvedAccount, Status> resolve_account(std::span<const std::uint8_t> sid,
                                                       RequestLog& trace);
Status map_error(security::LogonSecretError error);
Status map_profile_error(security::FaceProfileStoreError error);
std::optional<std::string_view> request_payload(const Request& request);
bool copy_payload(std::string_view payload, Response& response);
std::string fixed_utf8(const std::uint8_t* bytes, std::size_t capacity);
std::string json_escape(std::string_view text);
bool verify_windows_password(const ResolvedAccount& account, std::wstring_view password,
                             RequestLog& trace);
std::expected<std::vector<float>, DWORD>
run_recognition_agent(HANDLE stop_event, HANDLE client_gone, std::uint32_t requested_session_id,
                      const RecognitionSettings& settings, RequestLog& trace);
std::string embedding_source(std::span<const float> feature);
Status process_request(HANDLE stop_event, HANDLE client_gone, const Request& request,
                       Response& response, security::LogonSecretStore& store,
                       security::FaceProfileStore& profile_store,
                       ManagementAuthorizer& management_authorizer, std::mutex& state_mutex,
                       std::span<const std::uint8_t> caller_sid, DWORD caller_process_id,
                       RequestLog& trace);
} // namespace su::windows::auth_service::detail
