#include "logon_secret_server.h"

#include "windows/logon_secret_protocol.h"

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace su::windows::auth_service {

// Diagnostic log (SYSTEM-writable). Diagnostic only.
void server_log(const char* message) {
    std::ofstream log(L"C:\\su-deploy\\authsvc.log", std::ios::app);
    log << message << "\n";
}

namespace {

using smile2unlock::logon_secret_ipc::Operation;
using smile2unlock::logon_secret_ipc::Request;
using smile2unlock::logon_secret_ipc::Response;
using smile2unlock::logon_secret_ipc::Status;

constexpr wchar_t kPipeSddl[] = L"D:P(A;;GA;;;SY)(A;;GRGW;;;AU)";

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
    const auto wait_result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
    if (wait_result == WAIT_OBJECT_0) {
        (void)CancelIoEx(pipe, &overlapped);
        (void)WaitForSingleObject(read_event.get(), INFINITE);
        return false;
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
};

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
    return ResolvedAccount{*canonical_utf8, kind};
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

Status process_request(
    HANDLE pipe,
    const Request& request,
    Response& response,
    security::LogonSecretStore& store) {
    if (request.magic != smile2unlock::logon_secret_ipc::kMagic
        || request.version != smile2unlock::logon_secret_ipc::kVersion
        || request.request_id == 0) {
        return Status::kInvalidRequest;
    }
    const auto requested_sid = fixed_wide(request.sid);
    if (!requested_sid) {
        return Status::kInvalidRequest;
    }
    const auto caller_sid = client_sid(pipe);
    if (!caller_sid) {
        return Status::kAccessDenied;
    }
    const auto caller_is_system = is_local_system(*caller_sid);
    if ((request.operation == Operation::kPrepare
            || request.operation == Operation::kMarkStale)
        && !caller_is_system) {
        return Status::kAccessDenied;
    }
    if (request.operation == Operation::kStore || request.operation == Operation::kClear) {
        const auto caller_sid_text = sid_string(*caller_sid);
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
        auto password = store.prepare(
            *sid_utf8, request.request_id, request.logon_session_id);
        if (!password || password->size() >= std::size(response.password)) {
            return password ? Status::kCorrupt : map_error(password.error());
        }
        std::copy_n(password->c_str(), password->size() + 1, response.password);
        response.password_length = static_cast<std::uint32_t>(password->size());
        return Status::kOk;
    }
    case Operation::kMarkStale: {
        const auto marked = store.mark_stale(*sid_utf8);
        return marked ? Status::kOk : map_error(marked.error());
    }
    case Operation::kStore: {
        const auto password = fixed_wide(request.password);
        const auto account = resolve_account(*caller_sid);
        if (!password || !account) {
            return account ? Status::kInvalidRequest : account.error();
        }
        if (password->empty()) {
            return Status::kInvalidRequest;
        }
        const auto stored = store.store(
            *sid_utf8,
            account->canonical_username,
            account->kind,
            std::span<const wchar_t>{password->data(), password->size()});
        return stored ? Status::kOk : map_error(stored.error());
    }
    case Operation::kClear: {
        const auto cleared = store.clear(*sid_utf8);
        return cleared ? Status::kOk : map_error(cleared.error());
    }
    }
    return Status::kInvalidRequest;
}

} // namespace

LogonSecretServer::LogonSecretServer(security::StorageKey storage_key)
    : storage_key_(std::move(storage_key)), secret_store_(storage_key_) {}

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
        if (bytes_read == sizeof(request)) {
            server_log("serve: request received");
            response.request_id = request.request_id;
            response.logon_session_id = request.logon_session_id;
            response.status = process_request(raw_pipe, request, response, secret_store_);
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
