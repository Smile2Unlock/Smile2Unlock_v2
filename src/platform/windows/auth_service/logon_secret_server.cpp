#include "logon_secret_server.h"

#include "logon_secret_protocol.h"
#include "recognition_agent_protocol.h"
#include "sid_rate_limiter.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace su::windows::auth_service {

class ManagementAuthorizer {
public:
    static constexpr auto kTokenBytes = std::size_t{32};
    static constexpr auto kTokenCharacters = kTokenBytes * 2;

    ~ManagementAuthorizer() {
        for (auto& [_, grant] : grants_) {
            SecureZeroMemory(grant.token.data(), grant.token.size());
        }
    }

    bool issue(
        std::wstring_view sid,
        DWORD process_id,
        wchar_t* output,
        std::size_t capacity) {
        if (sid.empty() || process_id == 0 || capacity <= kTokenCharacters) {
            return false;
        }
        auto token = std::array<std::uint8_t, kTokenBytes>{};
        if (BCryptGenRandom(
                nullptr, token.data(), static_cast<ULONG>(token.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            return false;
        }
        static constexpr wchar_t kHex[] = L"0123456789abcdef";
        for (auto index = std::size_t{0}; index < token.size(); ++index) {
            output[index * 2] = kHex[token[index] >> 4];
            output[index * 2 + 1] = kHex[token[index] & 0x0F];
        }
        output[kTokenCharacters] = L'\0';
        prune_expired();
        const auto grant_key = key(sid, process_id);
        if (const auto existing = grants_.find(grant_key); existing != grants_.end()) {
            SecureZeroMemory(existing->second.token.data(), existing->second.token.size());
            grants_.erase(existing);
        }
        grants_.insert_or_assign(
            grant_key,
            Grant{
                .token = token,
                .expires_at = std::chrono::steady_clock::now() + std::chrono::minutes{10},
            });
        SecureZeroMemory(token.data(), token.size());
        return true;
    }

    bool consume(
        std::wstring_view sid,
        DWORD process_id,
        std::wstring_view encoded_token) {
        if (encoded_token.size() != kTokenCharacters) {
            return false;
        }
        auto token = decode(encoded_token);
        if (!token) {
            return false;
        }
        prune_expired();
        const auto grant = grants_.find(key(sid, process_id));
        if (grant == grants_.end()) {
            SecureZeroMemory(token->data(), token->size());
            return false;
        }
        auto difference = std::uint8_t{0};
        for (auto index = std::size_t{0}; index < token->size(); ++index) {
            difference |= (*token)[index] ^ grant->second.token[index];
        }
        SecureZeroMemory(token->data(), token->size());
        if (difference != 0) {
            return false;
        }
        SecureZeroMemory(grant->second.token.data(), grant->second.token.size());
        grants_.erase(grant);
        return true;
    }

private:
    struct Grant {
        std::array<std::uint8_t, kTokenBytes> token{};
        std::chrono::steady_clock::time_point expires_at;
    };

    static std::wstring key(std::wstring_view sid, DWORD process_id) {
        return std::wstring{sid} + L":" + std::to_wstring(process_id);
    }

    static std::optional<std::array<std::uint8_t, kTokenBytes>> decode(
        std::wstring_view encoded) {
        const auto nibble = [](wchar_t value) -> std::optional<std::uint8_t> {
            if (value >= L'0' && value <= L'9') return value - L'0';
            if (value >= L'a' && value <= L'f') return value - L'a' + 10;
            if (value >= L'A' && value <= L'F') return value - L'A' + 10;
            return std::nullopt;
        };
        auto token = std::array<std::uint8_t, kTokenBytes>{};
        for (auto index = std::size_t{0}; index < token.size(); ++index) {
            const auto high = nibble(encoded[index * 2]);
            const auto low = nibble(encoded[index * 2 + 1]);
            if (!high || !low) {
                SecureZeroMemory(token.data(), token.size());
                return std::nullopt;
            }
            token[index] = static_cast<std::uint8_t>((*high << 4) | *low);
        }
        return token;
    }

    void prune_expired() {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(grants_, [now](auto& item) {
            if (item.second.expires_at > now) {
                return false;
            }
            SecureZeroMemory(item.second.token.data(), item.second.token.size());
            return true;
        });
    }

    std::unordered_map<std::wstring, Grant> grants_;
};

// Diagnostic log (SYSTEM-writable). Diagnostic only.
void server_log(const char* message) {
    std::filesystem::create_directories(L"C:\\ProgramData\\Smile2Unlock\\Logs");
    std::ofstream log(
        L"C:\\ProgramData\\Smile2Unlock\\Logs\\auth-service.log", std::ios::app);
    log << message << "\n";
}

namespace {

using smile2unlock::logon_secret_ipc::Operation;
using smile2unlock::logon_secret_ipc::Request;
using smile2unlock::logon_secret_ipc::Response;
using smile2unlock::logon_secret_ipc::Status;

constexpr wchar_t kPipeSddl[] = L"D:P(A;;GA;;;SY)(A;;GRGW;;;AU)";
constexpr auto kPipeRequestTimeoutMs = DWORD{15'000};

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
    if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)
        && GetLastError() != ERROR_PIPE_CONNECTED) {
        return std::unexpected(GetLastError());
    }
    return true;
}

std::expected<bool, DWORD> read_request(
    HANDLE pipe, HANDLE stop_event, Request& request, DWORD& bytes_read) {
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
    const auto wait_result = WaitForMultipleObjects(
        2, events, FALSE, kPipeRequestTimeoutMs);
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
    if (wait_result != WAIT_OBJECT_0 + 1
        || !GetOverlappedResult(pipe, &overlapped, &bytes_read, FALSE)) {
        return std::unexpected(GetLastError());
    }
    return true;
}

bool write_response(HANDLE pipe, const Response& response) {
    const auto write_event = ScopedHandle{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!write_event) {
        return false;
    }
    auto overlapped = OVERLAPPED{};
    overlapped.hEvent = write_event.get();
    auto bytes_written = DWORD{0};
    if (!WriteFile(pipe, &response, sizeof(response), &bytes_written, &overlapped)) {
        if (GetLastError() != ERROR_IO_PENDING
            || WaitForSingleObject(write_event.get(), 3000) != WAIT_OBJECT_0
            || !GetOverlappedResult(pipe, &overlapped, &bytes_written, FALSE)) {
            (void)CancelIoEx(pipe, &overlapped);
            (void)WaitForSingleObject(write_event.get(), INFINITE);
            return false;
        }
    }
    return bytes_written == sizeof(response);
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
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kPipeSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
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
    if (!GetTokenInformation(
            raw_token, TokenUser, token_data.data(), required, &required)) {
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
    return CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(), &size)
        && EqualSid(const_cast<std::uint8_t*>(sid.data()), system_sid.data());
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
};

std::optional<DWORD> registry_dword(HKEY key, const wchar_t* name) {
    auto type = DWORD{};
    auto value = DWORD{};
    auto size = DWORD{sizeof(value)};
    if (RegQueryValueExW(
            key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size)
        != ERROR_SUCCESS
        || type != REG_DWORD || size != sizeof(value)) {
        return std::nullopt;
    }
    return value;
}

RecognitionSettings recognition_settings(std::wstring_view sid) {
    auto settings = RecognitionSettings{};
    const auto path = std::wstring{sid} + L"\\Software\\Smile2Unlock\\Recognition";
    auto raw_key = HKEY{};
    if (RegOpenKeyExW(HKEY_USERS, path.c_str(), 0, KEY_QUERY_VALUE, &raw_key)
        != ERROR_SUCCESS) {
        return settings;
    }
    const auto key = std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)>{
        raw_key, &RegCloseKey};
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
    return settings;
}

std::expected<ResolvedAccount, Status> resolve_account(
    std::span<const std::uint8_t> sid) {
    auto name_size = DWORD{0};
    auto domain_size = DWORD{0};
    auto sid_type = SID_NAME_USE{};
    (void)LookupAccountSidW(
        nullptr,
        const_cast<std::uint8_t*>(sid.data()),
        nullptr,
        &name_size,
        nullptr,
        &domain_size,
        &sid_type);
    if (name_size == 0 || domain_size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return std::unexpected(Status::kInvalidRequest);
    }
    auto name = std::wstring(name_size, L'\0');
    auto domain = std::wstring(domain_size, L'\0');
    if (!LookupAccountSidW(
            nullptr,
            const_cast<std::uint8_t*>(sid.data()),
            name.data(),
            &name_size,
            domain.data(),
            &domain_size,
            &sid_type)
        || sid_type != SidTypeUser) {
        return std::unexpected(Status::kInvalidRequest);
    }
    name.resize(wcsnlen(name.c_str(), name.size()));
    domain.resize(wcsnlen(domain.c_str(), domain.size()));

    auto computer_name = std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1>{};
    auto computer_name_size = DWORD{computer_name.size()};
    if (!GetComputerNameW(computer_name.data(), &computer_name_size)) {
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
    const auto required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::unexpected(Status::kInvalidRequest);
    }
    auto output = std::string(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            output.data(), required, nullptr, nullptr) != required) {
        return std::unexpected(Status::kInvalidRequest);
    }
    return output;
}

template <std::size_t N>
std::optional<std::wstring_view> fixed_wide(const wchar_t (&value)[N]) {
    const auto length = wcsnlen(value, N);
    if (length == 0 || length >= N) {
        return std::nullopt;
    }
    return std::wstring_view{value, length};
}

Status map_error(security::LogonSecretError error) {
    switch (error) {
    case security::LogonSecretError::kInvalidArgument: return Status::kInvalidRequest;
    case security::LogonSecretError::kAccessDenied: return Status::kStaleOrConsumed;
    case security::LogonSecretError::kCorrupt: return Status::kCorrupt;
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
    if (request.payload_length == 0
        || request.payload_length >= std::size(request.payload)) {
        return std::nullopt;
    }
    const auto payload = std::string_view{
        reinterpret_cast<const char*>(request.payload), request.payload_length};
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
    return std::string{reinterpret_cast<const char*>(bytes),
        static_cast<std::size_t>(end - bytes)};
}

std::string json_escape(std::string_view text) {
    auto escaped = std::string{};
    escaped.reserve(text.size());
    for (const auto character : text) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
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

bool verify_windows_password(
    const ResolvedAccount& account,
    std::wstring_view password) {
    auto password_buffer = std::vector<wchar_t>{password.begin(), password.end()};
    password_buffer.push_back(L'\0');
    HANDLE token = nullptr;
    const auto authenticated = LogonUserW(
        account.username.c_str(),
        account.domain.c_str(),
        password_buffer.data(),
        LOGON32_LOGON_NETWORK,
        LOGON32_PROVIDER_DEFAULT,
        &token);
    SecureZeroMemory(
        password_buffer.data(), password_buffer.size() * sizeof(wchar_t));
    if (token != nullptr) {
        CloseHandle(token);
    }
    return authenticated != FALSE;
}

bool write_exact(HANDLE handle, const void* input, DWORD size) {
    const auto* bytes = static_cast<const std::byte*>(input);
    DWORD total = 0;
    while (total < size) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes + total, size - total, &written, nullptr)
            || written == 0) {
            return false;
        }
        total += written;
    }
    return true;
}

std::filesystem::path sibling_path(const wchar_t* name) {
    auto path = std::wstring(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    path.resize(length);
    auto result = std::filesystem::path{path}.parent_path();
    result /= name;
    return result;
}

std::expected<std::vector<float>, DWORD> run_recognition_agent(
    HANDLE stop_event,
    std::uint32_t requested_session_id,
    const RecognitionSettings& settings) {
    using namespace smile2unlock::recognition_agent_ipc;
    using AgentRequest = smile2unlock::recognition_agent_ipc::Request;
    using AgentResponse = smile2unlock::recognition_agent_ipc::Response;
    const auto agent_path = sibling_path(L"su_recognition_agent.exe");
    const auto attributes = GetFileAttributesW(agent_path.c_str());
    if (agent_path.empty() || attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return std::unexpected(ERROR_FILE_NOT_FOUND);
    }

    auto inheritable = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };
    HANDLE child_read_raw = nullptr;
    HANDLE service_write_raw = nullptr;
    HANDLE service_read_raw = nullptr;
    HANDLE child_write_raw = nullptr;
    if (!CreatePipe(&child_read_raw, &service_write_raw, &inheritable, sizeof(AgentRequest))
        || !CreatePipe(&service_read_raw, &child_write_raw, &inheritable, sizeof(AgentResponse))) {
        if (child_read_raw != nullptr) CloseHandle(child_read_raw);
        if (service_write_raw != nullptr) CloseHandle(service_write_raw);
        if (service_read_raw != nullptr) CloseHandle(service_read_raw);
        if (child_write_raw != nullptr) CloseHandle(child_write_raw);
        return std::unexpected(GetLastError());
    }
    auto child_read = ScopedHandle{child_read_raw};
    auto service_write = ScopedHandle{service_write_raw};
    const auto service_read = ScopedHandle{service_read_raw};
    auto child_write = ScopedHandle{child_write_raw};
    if (!SetHandleInformation(service_write_raw, HANDLE_FLAG_INHERIT, 0)
        || !SetHandleInformation(service_read_raw, HANDLE_FLAG_INHERIT, 0)) {
        return std::unexpected(GetLastError());
    }

    const auto null_error_raw = CreateFileW(
        L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_error_raw == INVALID_HANDLE_VALUE) {
        return std::unexpected(GetLastError());
    }
    auto null_error = ScopedHandle{null_error_raw};

    HANDLE process_token_raw = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY,
            &process_token_raw)) {
        return std::unexpected(GetLastError());
    }
    const auto process_token = ScopedHandle{process_token_raw};
    HANDLE session_token_raw = nullptr;
    if (!DuplicateTokenEx(
            process_token_raw,
            TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY
                | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
            nullptr,
            SecurityImpersonation,
            TokenPrimary,
            &session_token_raw)) {
        return std::unexpected(GetLastError());
    }
    const auto session_token = ScopedHandle{session_token_raw};
    auto session_id = requested_session_id;
    if (session_id == 0) {
        session_id = WTSGetActiveConsoleSessionId();
    }
    if (session_id == 0 || session_id == 0xFFFF'FFFF
        || !SetTokenInformation(
            session_token_raw, TokenSessionId, &session_id, sizeof(session_id))) {
        return std::unexpected(session_id == 0 || session_id == 0xFFFF'FFFF
            ? ERROR_NO_SUCH_LOGON_SESSION : GetLastError());
    }

    auto attribute_size = SIZE_T{0};
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    auto attribute_storage = std::vector<std::byte>(attribute_size);
    auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_size)) {
        return std::unexpected(GetLastError());
    }
    using AttributeList = std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>;
    const auto delete_attributes = std::unique_ptr<AttributeList, decltype(&DeleteProcThreadAttributeList)>{
        attribute_list, &DeleteProcThreadAttributeList};
    const HANDLE inherited_handles[]{child_read_raw, child_write_raw, null_error_raw};
    if (!UpdateProcThreadAttribute(
            attribute_list,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(inherited_handles),
            sizeof(inherited_handles),
            nullptr,
            nullptr)) {
        return std::unexpected(GetLastError());
    }

    auto startup = STARTUPINFOEXW{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_read_raw;
    startup.StartupInfo.hStdOutput = child_write_raw;
    startup.StartupInfo.hStdError = null_error_raw;
    startup.lpAttributeList = attribute_list;
    auto command_line = L"\"" + agent_path.wstring() + L"\"";
    auto process = PROCESS_INFORMATION{};
    void* environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, session_token_raw, FALSE)) {
        return std::unexpected(GetLastError());
    }
    const auto destroy_environment = std::unique_ptr<void, decltype(&DestroyEnvironmentBlock)>{
        environment, &DestroyEnvironmentBlock};
    const auto working_directory = agent_path.parent_path();
    if (!CreateProcessAsUserW(
            session_token_raw,
            agent_path.c_str(),
            command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
            environment,
            working_directory.c_str(),
            &startup.StartupInfo,
            &process)) {
        return std::unexpected(GetLastError());
    }
    const auto process_handle = ScopedHandle{process.hProcess};
    const auto thread_handle = ScopedHandle{process.hThread};
    CloseHandle(child_read.release());
    CloseHandle(child_write.release());
    null_error.reset();

    const auto terminate_agent = [&](DWORD exit_code) {
        (void)TerminateProcess(process.hProcess, exit_code);
        (void)WaitForSingleObject(process.hProcess, 3000);
    };

    auto request = AgentRequest{};
    if (BCryptGenRandom(
            nullptr,
            request.nonce.data(),
            static_cast<ULONG>(request.nonce.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        terminate_agent(ERROR_GEN_FAILURE);
        return std::unexpected(ERROR_GEN_FAILURE);
    }
    request.camera_index = settings.camera_index;
    request.timeout_ms = 10'000;
    request.liveness_threshold = settings.liveness_detection
        ? settings.liveness_threshold
        : 0.0F;
    if (!write_exact(service_write_raw, &request, sizeof(request))) {
        const auto error = GetLastError();
        terminate_agent(ERROR_WRITE_FAULT);
        return std::unexpected(error);
    }
    service_write.reset();

    auto response = AgentResponse{};
    auto received = std::size_t{0};
    const auto deadline = GetTickCount64() + request.timeout_ms + 3000;
    while (received < sizeof(response)) {
        if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
            terminate_agent(ERROR_CANCELLED);
            SecureZeroMemory(&request, sizeof(request));
            SecureZeroMemory(&response, sizeof(response));
            return std::unexpected(ERROR_CANCELLED);
        }
        auto available = DWORD{0};
        if (!PeekNamedPipe(service_read_raw, nullptr, 0, nullptr, &available, nullptr)) {
            const auto error = GetLastError();
            terminate_agent(error);
            SecureZeroMemory(&request, sizeof(request));
            SecureZeroMemory(&response, sizeof(response));
            return std::unexpected(error);
        }
        if (available != 0) {
            const auto remaining = sizeof(response) - received;
            const auto to_read = static_cast<DWORD>(std::min<std::size_t>(available, remaining));
            auto bytes_read = DWORD{0};
            auto* destination = reinterpret_cast<std::byte*>(&response) + received;
            if (!ReadFile(service_read_raw, destination, to_read, &bytes_read, nullptr)
                || bytes_read == 0) {
                const auto error = GetLastError();
                terminate_agent(error);
                SecureZeroMemory(&request, sizeof(request));
                SecureZeroMemory(&response, sizeof(response));
                return std::unexpected(error);
            }
            received += bytes_read;
            continue;
        }
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            SecureZeroMemory(&request, sizeof(request));
            SecureZeroMemory(&response, sizeof(response));
            return std::unexpected(ERROR_HANDLE_EOF);
        }
        const auto now = GetTickCount64();
        if (now >= deadline) {
            terminate_agent(ERROR_TIMEOUT);
            SecureZeroMemory(&request, sizeof(request));
            SecureZeroMemory(&response, sizeof(response));
            return std::unexpected(ERROR_TIMEOUT);
        }
        (void)WaitForSingleObject(
            stop_event, static_cast<DWORD>(std::min<ULONGLONG>(25, deadline - now)));
    }

    auto exit_code = DWORD{STILL_ACTIVE};
    if (WaitForSingleObject(process.hProcess, 3000) != WAIT_OBJECT_0
        || !GetExitCodeProcess(process.hProcess, &exit_code)
        || exit_code != ERROR_SUCCESS) {
        terminate_agent(ERROR_GEN_FAILURE);
        SecureZeroMemory(&request, sizeof(request));
        SecureZeroMemory(&response, sizeof(response));
        return std::unexpected(ERROR_GEN_FAILURE);
    }
    if (response.magic != kResponseMagic
        || response.version != kVersion
        || response.nonce != request.nonce
        || response.status != AgentStatus::kOk
        || response.feature_count == 0
        || response.feature_count > response.feature.size()
        || !std::isfinite(response.liveness_score)
        || response.liveness_score < request.liveness_threshold) {
        SecureZeroMemory(&request, sizeof(request));
        SecureZeroMemory(&response, sizeof(response));
        return std::unexpected(ERROR_ACCESS_DENIED);
    }
    auto feature = std::vector<float>(
        response.feature.begin(), response.feature.begin() + response.feature_count);
    SecureZeroMemory(&request, sizeof(request));
    SecureZeroMemory(&response, sizeof(response));
    return feature;
}

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

Status process_request(
    HANDLE stop_event,
    const Request& request,
    Response& response,
    security::LogonSecretStore& store,
    security::FaceProfileStore& profile_store,
    ManagementAuthorizer& management_authorizer,
    std::span<const std::uint8_t> caller_sid,
    DWORD caller_process_id) {
    if (request.magic != smile2unlock::logon_secret_ipc::kMagic
        || request.version != smile2unlock::logon_secret_ipc::kVersion
        || request.request_id == 0) {
        return Status::kInvalidRequest;
    }
    const auto requested_sid = fixed_wide(request.sid);
    if (!requested_sid) {
        return Status::kInvalidRequest;
    }
    const auto caller_is_system = is_local_system(caller_sid);
    if ((request.operation == Operation::kPrepare
            || request.operation == Operation::kAuthenticateAndPrepare
            || request.operation == Operation::kMarkStale)
        && !caller_is_system) {
        return Status::kAccessDenied;
    }
    if (request.operation == Operation::kStore
        || request.operation == Operation::kClear
        || request.operation == Operation::kEnrollProfile
        || request.operation == Operation::kListProfiles
        || request.operation == Operation::kDeleteProfile
        || request.operation == Operation::kVerifyProfile
        || request.operation == Operation::kCredentialStatus) {
        const auto caller_sid_text = sid_string(caller_sid);
        if (!caller_sid_text || !std::ranges::equal(*caller_sid_text, *requested_sid)) {
            return Status::kAccessDenied;
        }
    }
    const auto sid_utf8 = wide_to_utf8(*requested_sid);
    if (!sid_utf8) {
        return sid_utf8.error();
    }

    switch (request.operation) {
    case Operation::kPrepare: {
        // Version 2 closes the old bypass: password release is only reachable
        // through kAuthenticateAndPrepare after service-owned face matching.
        return Status::kAccessDenied;
    }
    case Operation::kMarkStale: {
        const auto marked = store.mark_stale(*sid_utf8);
        return marked ? Status::kOk : map_error(marked.error());
    }
    case Operation::kStore: {
        const auto password = fixed_wide(request.password);
        const auto account = resolve_account(caller_sid);
        if (!password || !account) {
            return account ? Status::kInvalidRequest : account.error();
        }
        if (password->empty()) {
            return Status::kInvalidRequest;
        }
        if (!verify_windows_password(*account, *password)) {
            return Status::kAuthenticationFailed;
        }
        const auto stored = store.store(
            *sid_utf8,
            account->canonical_username,
            account->kind,
            std::span<const wchar_t>{password->data(), password->size()});
        if (!stored) {
            return map_error(stored.error());
        }
        if (!management_authorizer.issue(
                *requested_sid, caller_process_id,
                response.password, std::size(response.password))) {
            return Status::kUnavailable;
        }
        response.password_length = ManagementAuthorizer::kTokenCharacters;
        return Status::kOk;
    }
    case Operation::kClear: {
        const auto password = fixed_wide(request.password);
        const auto account = resolve_account(caller_sid);
        if (!password || !account) {
            return account ? Status::kInvalidRequest : account.error();
        }
        if (!verify_windows_password(*account, *password)) {
            return Status::kAuthenticationFailed;
        }
        const auto cleared = store.clear(*sid_utf8);
        return cleared ? Status::kOk : map_error(cleared.error());
    }
    case Operation::kAuthenticateAndPrepare: {
        const auto settings = recognition_settings(*requested_sid);
        const auto feature = run_recognition_agent(
            stop_event, request.logon_session_id, settings);
        if (!feature) {
            return Status::kAuthenticationFailed;
        }
        const auto source = embedding_source(*feature);
        const auto report = profile_store.authenticate(
            *sid_utf8, source, settings.recognition_threshold, true);
        if (!report) {
            return map_profile_error(report.error());
        }
        if (!report->accepted || !report->liveness_ok) {
            return Status::kAuthenticationFailed;
        }
        auto password = store.prepare(
            *sid_utf8, request.request_id, request.logon_session_id);
        if (!password || password->size() >= std::size(response.password)) {
            return password ? Status::kCorrupt : map_error(password.error());
        }
        std::copy_n(password->c_str(), password->size() + 1, response.password);
        response.password_length = static_cast<std::uint32_t>(password->size());
        return Status::kOk;
    }
    case Operation::kEnrollProfile: {
        const auto label = fixed_wide(request.canonical_username);
        const auto management_token = fixed_wide(request.password);
        const auto payload = request_payload(request);
        if (!label || !management_token || !payload) {
            return Status::kInvalidRequest;
        }
        if (!management_authorizer.consume(
                *requested_sid, caller_process_id, *management_token)) {
            return Status::kAccessDenied;
        }
        const auto label_utf8 = wide_to_utf8(*label);
        if (!label_utf8) {
            return label_utf8.error();
        }
        const auto credential_ready = store.configured(*sid_utf8);
        if (!credential_ready || !*credential_ready) {
            return credential_ready ? Status::kUnavailable : map_error(credential_ready.error());
        }
        const auto enrolled = profile_store.enroll(*sid_utf8, *label_utf8, *payload);
        if (!enrolled) {
            return map_profile_error(enrolled.error());
        }
        const auto profiles = profile_store.list_json(*sid_utf8);
        if (!profiles || !copy_payload(*profiles, response)) {
            return profiles ? Status::kCorrupt : map_profile_error(profiles.error());
        }
        if (!management_authorizer.issue(
                *requested_sid, caller_process_id,
                response.password, std::size(response.password))) {
            return Status::kUnavailable;
        }
        response.password_length = ManagementAuthorizer::kTokenCharacters;
        return Status::kOk;
    }
    case Operation::kListProfiles: {
        const auto profiles = profile_store.list_json(*sid_utf8);
        return profiles && copy_payload(*profiles, response)
            ? Status::kOk
            : (profiles ? Status::kCorrupt : map_profile_error(profiles.error()));
    }
    case Operation::kDeleteProfile: {
        const auto management_token = fixed_wide(request.password);
        const auto profile_id = request_payload(request);
        if (!management_token || !profile_id) {
            return Status::kInvalidRequest;
        }
        if (!management_authorizer.consume(
                *requested_sid, caller_process_id, *management_token)) {
            return Status::kAccessDenied;
        }
        const auto removed = profile_store.remove(*sid_utf8, *profile_id);
        if (!removed) {
            return map_profile_error(removed.error());
        }
        if (!*removed) {
            return Status::kProfileNotFound;
        }
        if (!management_authorizer.issue(
                *requested_sid, caller_process_id,
                response.password, std::size(response.password))) {
            return Status::kUnavailable;
        }
        response.password_length = ManagementAuthorizer::kTokenCharacters;
        return Status::kOk;
    }
    case Operation::kVerifyProfile: {
        const auto payload = request_payload(request);
        if (!payload) {
            return Status::kInvalidRequest;
        }
        const auto report = profile_store.authenticate(
            *sid_utf8, *payload, 0.65F, request.account_kind == 1);
        if (!report) {
            return map_profile_error(report.error());
        }
        const auto report_json = std::format(
            R"({{"accepted":{},"score":{},"threshold":{},"liveness_ok":{},"profile_count":{},"best_profile_id":"{}","best_profile_label":"{}","reason":"{}"}})",
            report->accepted ? "true" : "false",
            report->score,
            report->threshold,
            report->liveness_ok ? "true" : "false",
            report->profile_count,
            json_escape(fixed_utf8(report->best_profile_id, std::size(report->best_profile_id))),
            json_escape(fixed_utf8(report->best_profile_label, std::size(report->best_profile_label))),
            json_escape(fixed_utf8(report->reason, std::size(report->reason))));
        return copy_payload(report_json, response) ? Status::kOk : Status::kCorrupt;
    }
    case Operation::kCredentialStatus: {
        const auto configured = store.configured(*sid_utf8);
        if (!configured) {
            return map_error(configured.error());
        }
        response.payload[0] = *configured ? 1 : 0;
        response.payload_length = 1;
        return Status::kOk;
    }
    }
    return Status::kInvalidRequest;
}

} // namespace

LogonSecretServer::LogonSecretServer(security::StorageKey storage_key)
    : storage_key_(std::move(storage_key)),
      secret_store_(storage_key_),
      profile_store_(storage_key_),
      management_authorizer_(std::make_unique<ManagementAuthorizer>()),
      rate_limiter_(std::make_unique<SidRateLimiter>()) {}

LogonSecretServer::~LogonSecretServer() = default;

std::expected<void, DWORD> LogonSecretServer::serve(HANDLE stop_event) {
    const auto descriptor = pipe_descriptor();
    if (!descriptor) {
        return std::unexpected(descriptor.error());
    }
    auto attributes = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor->get(),
        .bInheritHandle = FALSE,
    };
    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        const auto raw_pipe = CreateNamedPipeW(
            smile2unlock::logon_secret_ipc::kPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            sizeof(Response),
            sizeof(Request),
            3000,
            &attributes);
        if (raw_pipe == INVALID_HANDLE_VALUE) {
            server_log("serve: CreateNamedPipeW FAILED");
            return std::unexpected(GetLastError());
        }
        server_log("serve: pipe created, waiting for client");
        const auto pipe = ScopedHandle{raw_pipe};
        const auto connected = wait_for_pipe_client(raw_pipe, stop_event);
        if (!connected) {
            if (connected.error() == ERROR_OPERATION_ABORTED
                || WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }
        if (!*connected) {
            break;
        }
        server_log("serve: client connected");

        const auto caller_sid = client_sid(raw_pipe);
        const auto caller_process_id = client_process_id(raw_pipe);
        auto caller_sid_text = std::expected<std::wstring, DWORD>{
            std::unexpected(ERROR_ACCESS_DENIED)};
        if (caller_sid) {
            caller_sid_text = sid_string(*caller_sid);
        }
        if (!caller_sid || !caller_process_id || !caller_sid_text
            || !rate_limiter_->allow_connection(*caller_sid_text)) {
            (void)DisconnectNamedPipe(raw_pipe);
            continue;
        }

        auto request = Request{};
        auto bytes_read = DWORD{0};
        auto response = Response{};
        const auto read = read_request(raw_pipe, stop_event, request, bytes_read);
        if (!read || !*read) {
            smile2unlock::logon_secret_ipc::clear_request(request);
            if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }
        if (bytes_read == sizeof(request)
            && rate_limiter_->allow_request(*caller_sid_text)) {
            server_log("serve: request received");
            response.request_id = request.request_id;
            response.logon_session_id = request.logon_session_id;
            response.status = process_request(
                stop_event, request, response, secret_store_, profile_store_,
                *management_authorizer_, *caller_sid, *caller_process_id);
            server_log(("serve: response status=" + std::to_string(static_cast<int>(response.status))).c_str());
        } else {
            response.status = Status::kInvalidRequest;
        }
        smile2unlock::logon_secret_ipc::clear_request(request);
        (void)write_response(raw_pipe, response);
        (void)FlushFileBuffers(raw_pipe);
        smile2unlock::logon_secret_ipc::clear_response(response);
        (void)DisconnectNamedPipe(raw_pipe);
    }
    return {};
}

} // namespace su::windows::auth_service
