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
constexpr wchar_t kPipeSddl[] = L"D:P(A;;GA;;;SY)(A;;GRGW;;;AU)";
constexpr auto kPipeRequestTimeoutMs = DWORD{15'000};
std::expected<bool, DWORD> wait_for_pipe_client(HANDLE pipe, HANDLE stop_event) {
    const auto connect_event = ScopedHandle{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!connect_event) {
        return std::unexpected(GetLastError());
    }
    auto overlapped = OVERLAPPED{};
    overlapped.hEvent = connect_event.get();
    if (ConnectNamedPipe(pipe, &overlapped)) {
        return true;
    }
    const auto connect_error = GetLastError();
    if (connect_error == ERROR_PIPE_CONNECTED) {
        return true;
    }
    if (connect_error != ERROR_IO_PENDING) {
        return std::unexpected(connect_error);
    }
    const HANDLE events[]{stop_event, connect_event.get()};
    const auto wait_result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
    if (wait_result == WAIT_OBJECT_0) {
        (void)CancelIoEx(pipe, &overlapped);
        (void)WaitForSingleObject(connect_event.get(), INFINITE);
        return false;
    }
    if (wait_result != WAIT_OBJECT_0 + 1) {
        (void)CancelIoEx(pipe, &overlapped);
        (void)WaitForSingleObject(connect_event.get(), INFINITE);
        return std::unexpected(ERROR_GEN_FAILURE);
    }
    auto transferred = DWORD{0};
    if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) &&
        GetLastError() != ERROR_PIPE_CONNECTED) {
        return std::unexpected(GetLastError());
    }
    return true;
}

std::expected<bool, DWORD> read_request(HANDLE pipe, HANDLE stop_event, Request& request,
                                        DWORD& bytes_read) {
    const auto read_event = ScopedHandle{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!read_event) {
        return std::unexpected(GetLastError());
    }
    auto overlapped = OVERLAPPED{};
    overlapped.hEvent = read_event.get();
    if (ReadFile(pipe, &request, sizeof(request), &bytes_read, &overlapped)) {
        return true;
    }
    const auto read_error = GetLastError();
    if (read_error != ERROR_IO_PENDING) {
        return std::unexpected(read_error);
    }
    const HANDLE events[]{stop_event, read_event.get()};
    const auto wait_result = WaitForMultipleObjects(2, events, FALSE, kPipeRequestTimeoutMs);
    if (wait_result == WAIT_OBJECT_0) {
        (void)CancelIoEx(pipe, &overlapped);
        (void)WaitForSingleObject(read_event.get(), INFINITE);
        return false;
    }
    if (wait_result == WAIT_TIMEOUT) {
        (void)CancelIoEx(pipe, &overlapped);
        (void)WaitForSingleObject(read_event.get(), INFINITE);
        return std::unexpected(ERROR_TIMEOUT);
    }
    if (wait_result != WAIT_OBJECT_0 + 1 ||
        !GetOverlappedResult(pipe, &overlapped, &bytes_read, FALSE)) {
        return std::unexpected(GetLastError());
    }
    return true;
}

class RevertGuard {
public:
    RevertGuard() = default;
    ~RevertGuard() { RevertToSelf(); }
    RevertGuard(const RevertGuard&) = delete;
    RevertGuard& operator=(const RevertGuard&) = delete;
};

std::expected<LocalMemory, DWORD> pipe_descriptor() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddl, SDDL_REVISION_1,
                                                              &descriptor, nullptr)) {
        return std::unexpected(GetLastError());
    }
    return LocalMemory{descriptor};
}

std::expected<std::vector<std::uint8_t>, DWORD> client_sid(HANDLE pipe) {
    if (!ImpersonateNamedPipeClient(pipe)) {
        return std::unexpected(GetLastError());
    }
    const auto revert = RevertGuard{};
    auto raw_token = HANDLE{};
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token)) {
        return std::unexpected(GetLastError());
    }
    const auto token = ScopedHandle{raw_token};
    auto required = DWORD{0};
    GetTokenInformation(raw_token, TokenUser, nullptr, 0, &required);
    if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return std::unexpected(GetLastError());
    }
    auto token_data = std::vector<std::uint8_t>(required);
    if (!GetTokenInformation(raw_token, TokenUser, token_data.data(), required, &required)) {
        return std::unexpected(GetLastError());
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(token_data.data());
    const auto sid_size = GetLengthSid(user->User.Sid);
    auto sid = std::vector<std::uint8_t>(sid_size);
    if (!CopySid(sid_size, sid.data(), user->User.Sid)) {
        return std::unexpected(GetLastError());
    }
    return sid;
}

std::expected<DWORD, DWORD> client_process_id(HANDLE pipe) {
    auto process_id = ULONG{};
    if (!GetNamedPipeClientProcessId(pipe, &process_id) || process_id == 0) {
        return std::unexpected(GetLastError());
    }
    return static_cast<DWORD>(process_id);
}

bool is_local_system(std::span<const std::uint8_t> sid) {
    auto system_sid = std::array<std::uint8_t, SECURITY_MAX_SID_SIZE>{};
    auto size = DWORD{system_sid.size()};
    return CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(), &size) &&
           EqualSid(const_cast<std::uint8_t*>(sid.data()), system_sid.data());
}

std::expected<std::wstring, DWORD> sid_string(std::span<const std::uint8_t> sid) {
    LPWSTR raw_sid = nullptr;
    if (!ConvertSidToStringSidW(const_cast<std::uint8_t*>(sid.data()), &raw_sid)) {
        return std::unexpected(GetLastError());
    }
    const auto free_sid = LocalMemory{raw_sid};
    return std::wstring{raw_sid};
}

std::expected<std::string, Status> wide_to_utf8(std::wstring_view value);

std::optional<DWORD> registry_dword(HKEY key, const wchar_t* name) {
    auto type = DWORD{};
    auto value = DWORD{};
    auto size = DWORD{sizeof(value)};
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) !=
            ERROR_SUCCESS ||
        type != REG_DWORD || size != sizeof(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<RecognitionSettings> recognition_settings_at(HKEY root, const std::wstring& path) {
    auto raw_key = HKEY{};
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE, &raw_key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    const auto key =
        std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)>{raw_key, &RegCloseKey};
    auto settings = RecognitionSettings{};
    if (const auto value = registry_dword(raw_key, L"CameraIndex"); value && *value <= 64) {
        settings.camera_index = static_cast<std::int32_t>(*value);
    }
    if (const auto value = registry_dword(raw_key, L"RecognitionThresholdMilli");
        value && *value >= 500 && *value <= 1000) {
        settings.recognition_threshold = static_cast<float>(*value) / 1000.0F;
    }
    if (const auto value = registry_dword(raw_key, L"LivenessEnabled"); value && *value <= 1) {
        settings.liveness_detection = *value != 0;
    }
    if (const auto value = registry_dword(raw_key, L"LivenessThresholdMilli");
        value && *value >= 300 && *value <= 1000) {
        settings.liveness_threshold = static_cast<float>(*value) / 1000.0F;
    }
    if (const auto value = registry_dword(raw_key, L"TimeoutSec");
        value && *value >= 5 && *value <= 600) {
        settings.timeout_ms = *value * 1000;
    }
    return settings;
}

// The machine-wide key written by kSetRecognitionSettings is authoritative so
// a cold boot (user hive not loaded) still sees the configured policy. The
// per-user key remains a fallback for installs that predate the service store.
RecognitionSettings recognition_settings(std::wstring_view sid) {
    if (const auto settings = recognition_settings_at(
            HKEY_LOCAL_MACHINE, L"SOFTWARE\\Smile2Unlock\\Recognition\\" + std::wstring{sid})) {
        return *settings;
    }
    if (const auto settings = recognition_settings_at(
            HKEY_USERS, std::wstring{sid} + L"\\Software\\Smile2Unlock\\Recognition")) {
        return *settings;
    }
    return {};
}

bool valid_recognition_settings(
    const smile2unlock::logon_secret_ipc::RecognitionSettingsPayload& payload) {
    return payload.camera_index <= 64 && payload.recognition_threshold_milli >= 500 &&
           payload.recognition_threshold_milli <= 1000 && payload.liveness_enabled <= 1 &&
           payload.liveness_threshold_milli >= 300 && payload.liveness_threshold_milli <= 1000 &&
           payload.recognition_mode <= 1 && payload.auto_delay_sec <= 3600 &&
           payload.retry_delay_sec >= 1 && payload.retry_delay_sec <= 3600 &&
           payload.timeout_sec >= 5 && payload.timeout_sec <= 600;
}

bool write_recognition_settings(
    std::wstring_view sid,
    const smile2unlock::logon_secret_ipc::RecognitionSettingsPayload& payload, RequestLog& trace) {
    const auto path = L"SOFTWARE\\Smile2Unlock\\Recognition\\" + std::wstring{sid};
    auto raw_key = HKEY{};
    const auto created =
        RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &raw_key, nullptr);
    if (created != ERROR_SUCCESS) {
        trace.cause(static_cast<DWORD>(created));
        return false;
    }
    const auto key =
        std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)>{raw_key, &RegCloseKey};
    const auto set = [raw_key, &trace](const wchar_t* name, DWORD value) {
        const auto result = RegSetValueExW(raw_key, name, 0, REG_DWORD,
                                           reinterpret_cast<const BYTE*>(&value), sizeof(value));
        if (result != ERROR_SUCCESS)
            trace.cause(static_cast<DWORD>(result));
        return result == ERROR_SUCCESS;
    };
    return set(L"CameraIndex", payload.camera_index) &&
           set(L"RecognitionThresholdMilli", payload.recognition_threshold_milli) &&
           set(L"LivenessEnabled", payload.liveness_enabled) &&
           set(L"LivenessThresholdMilli", payload.liveness_threshold_milli) &&
           set(L"RecognitionMode", payload.recognition_mode) &&
           set(L"AutoDelaySec", payload.auto_delay_sec) &&
           set(L"RetryDelaySec", payload.retry_delay_sec) &&
           set(L"TimeoutSec", payload.timeout_sec);
}

std::expected<ResolvedAccount, Status> resolve_account(std::span<const std::uint8_t> sid,
                                                       RequestLog& trace) {
    auto name_size = DWORD{0};
    auto domain_size = DWORD{0};
    auto sid_type = SID_NAME_USE{};
    (void)LookupAccountSidW(nullptr, const_cast<std::uint8_t*>(sid.data()), nullptr, &name_size,
                            nullptr, &domain_size, &sid_type);
    if (name_size == 0 || domain_size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        trace.cause(GetLastError());
        return std::unexpected(Status::kInvalidRequest);
    }
    auto name = std::wstring(name_size, L'\0');
    auto domain = std::wstring(domain_size, L'\0');
    if (!LookupAccountSidW(nullptr, const_cast<std::uint8_t*>(sid.data()), name.data(), &name_size,
                           domain.data(), &domain_size, &sid_type) ||
        sid_type != SidTypeUser) {
        trace.cause(GetLastError());
        return std::unexpected(Status::kInvalidRequest);
    }
    name.resize(wcsnlen(name.c_str(), name.size()));
    domain.resize(wcsnlen(domain.c_str(), domain.size()));

    auto computer_name = std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1>{};
    auto computer_name_size = DWORD{computer_name.size()};
    if (!GetComputerNameW(computer_name.data(), &computer_name_size)) {
        trace.cause(GetLastError());
        return std::unexpected(Status::kUnavailable);
    }
    auto kind = SuWindowsAccountKind_Local;
    if (_wcsicmp(domain.c_str(), L"MicrosoftAccount") == 0) {
        kind = SuWindowsAccountKind_Microsoft;
    } else if (_wcsicmp(domain.c_str(), computer_name.data()) != 0) {
        // Domain and Entra accounts remain disabled until separately validated.
        return std::unexpected(Status::kAccessDenied);
    }
    const auto canonical = domain + L"\\" + name;
    const auto canonical_utf8 = wide_to_utf8(canonical);
    if (!canonical_utf8) {
        return std::unexpected(canonical_utf8.error());
    }
    return ResolvedAccount{*canonical_utf8, kind, std::move(domain), std::move(name)};
}

std::expected<std::string, Status> wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return std::unexpected(Status::kInvalidRequest);
    }
    const auto required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::unexpected(Status::kInvalidRequest);
    }
    auto output = std::string(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), output.data(), required, nullptr,
                            nullptr) != required) {
        return std::unexpected(Status::kInvalidRequest);
    }
    return output;
}

Status map_error(security::LogonSecretError error) {
    switch (error) {
    case security::LogonSecretError::kInvalidArgument:
        return Status::kInvalidRequest;
    case security::LogonSecretError::kAccessDenied:
        return Status::kStaleOrConsumed;
    case security::LogonSecretError::kCorrupt:
        return Status::kCorrupt;
    case security::LogonSecretError::kUnavailable:
    case security::LogonSecretError::kWriteFailed:
        return Status::kUnavailable;
    }
    return Status::kUnavailable;
}

Status map_profile_error(security::FaceProfileStoreError error) {
    switch (error) {
    case security::FaceProfileStoreError::kInvalidArgument:
        return Status::kInvalidRequest;
    case security::FaceProfileStoreError::kCorrupt:
        return Status::kCorrupt;
    case security::FaceProfileStoreError::kNotFound:
        return Status::kProfileNotFound;
    case security::FaceProfileStoreError::kUnavailable:
    case security::FaceProfileStoreError::kWriteFailed:
        return Status::kUnavailable;
    }
    return Status::kUnavailable;
}

std::optional<std::string_view> request_payload(const Request& request) {
    if (request.payload_length == 0 || request.payload_length >= std::size(request.payload)) {
        return std::nullopt;
    }
    const auto payload =
        std::string_view{reinterpret_cast<const char*>(request.payload), request.payload_length};
    if (payload.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    return payload;
}

bool copy_payload(std::string_view payload, Response& response) {
    if (payload.size() >= std::size(response.payload)) {
        return false;
    }
    std::ranges::copy(payload, response.payload);
    response.payload[payload.size()] = 0;
    response.payload_length = static_cast<std::uint32_t>(payload.size());
    return true;
}

std::string fixed_utf8(const std::uint8_t* bytes, std::size_t capacity) {
    const auto* end = std::find(bytes, bytes + capacity, std::uint8_t{0});
    return std::string{reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(end - bytes)};
}

std::string json_escape(std::string_view text) {
    auto escaped = std::string{};
    escaped.reserve(text.size());
    for (const auto character : text) {
        switch (character) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                escaped += '?';
            } else {
                escaped += character;
            }
        }
    }
    return escaped;
}

bool verify_windows_password(const ResolvedAccount& account, std::wstring_view password,
                             RequestLog& trace) {
    auto password_buffer = std::vector<wchar_t>{password.begin(), password.end()};
    password_buffer.push_back(L'\0');
    HANDLE token = nullptr;
    const auto authenticated =
        LogonUserW(account.username.c_str(), account.domain.c_str(), password_buffer.data(),
                   LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &token);
    const auto error = GetLastError();
    SecureZeroMemory(password_buffer.data(), password_buffer.size() * sizeof(wchar_t));
    if (token != nullptr) {
        CloseHandle(token);
    }
    if (!authenticated)
        trace.cause(error);
    return authenticated != FALSE;
}

} // namespace su::windows::auth_service::detail
