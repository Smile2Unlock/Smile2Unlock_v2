#include "logon_secret_client.h"

#include <sddl.h>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

using smile2unlock::logon_secret_ipc::Operation;
using smile2unlock::logon_secret_ipc::Request;
using smile2unlock::logon_secret_ipc::Response;
using smile2unlock::logon_secret_ipc::Status;

namespace {

HRESULT status_to_hresult(Status status) {
    switch (status) {
    case Status::kOk: return S_OK;
    case Status::kInvalidRequest: return E_INVALIDARG;
    case Status::kAccessDenied: return E_ACCESSDENIED;
    case Status::kStaleOrConsumed: return HRESULT_FROM_WIN32(ERROR_PASSWORD_RESTRICTION);
    case Status::kCorrupt: return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    case Status::kUnavailable: return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE);
    }
    return E_FAIL;
}

bool copy_fixed(PCWSTR value, wchar_t* output, std::size_t capacity) {
    if (value == nullptr || value[0] == L'\0') {
        return false;
    }
    const auto length = wcsnlen(value, capacity);
    if (length == 0 || length >= capacity) {
        return false;
    }
    std::copy_n(value, length, output);
    output[length] = L'\0';
    return true;
}

} // namespace

std::expected<std::wstring, HRESULT> LogonSecretClient::current_user_sid() const {
    auto raw_token = HANDLE{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
    }
    const auto token = std::unique_ptr<void, decltype(&CloseHandle)>{raw_token, &CloseHandle};
    auto required = DWORD{0};
    GetTokenInformation(raw_token, TokenUser, nullptr, 0, &required);
    if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
    }
    auto token_data = std::vector<std::uint8_t>(required);
    if (!GetTokenInformation(
            raw_token, TokenUser, token_data.data(), required, &required)) {
        return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
    }
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(token_data.data());
    LPWSTR raw_sid = nullptr;
    if (!ConvertSidToStringSidW(token_user->User.Sid, &raw_sid)) {
        return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
    }
    const auto sid = std::wstring{raw_sid};
    LocalFree(raw_sid);
    return sid;
}

PreparedPipePassword::PreparedPipePassword(PreparedPipePassword&& other) noexcept
    : password_(other.password_), size_(std::exchange(other.size_, 0)) {
    other.clear();
}

PreparedPipePassword& PreparedPipePassword::operator=(PreparedPipePassword&& other) noexcept {
    if (this != &other) {
        clear();
        password_ = other.password_;
        size_ = std::exchange(other.size_, 0);
        other.clear();
    }
    return *this;
}

PreparedPipePassword::~PreparedPipePassword() {
    clear();
}

void PreparedPipePassword::clear() noexcept {
    SecureZeroMemory(password_.data(), password_.size() * sizeof(password_[0]));
    size_ = 0;
}

std::expected<Response, HRESULT> LogonSecretClient::transact(Request& request) const {
    auto response = Response{};
    auto bytes_read = DWORD{0};
    const auto called = CallNamedPipeW(
        smile2unlock::logon_secret_ipc::kPipeName,
        &request,
        sizeof(request),
        &response,
        sizeof(response),
        &bytes_read,
        3000);
    smile2unlock::logon_secret_ipc::clear_request(request);
    if (!called) {
        return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
    }
    if (bytes_read != sizeof(response)
        || response.magic != smile2unlock::logon_secret_ipc::kMagic
        || response.version != smile2unlock::logon_secret_ipc::kVersion
        || response.request_id == 0
        || response.request_id != request.request_id
        || response.logon_session_id != request.logon_session_id) {
        smile2unlock::logon_secret_ipc::clear_response(response);
        return std::unexpected(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    }
    if (response.status != Status::kOk) {
        const auto error = status_to_hresult(response.status);
        smile2unlock::logon_secret_ipc::clear_response(response);
        return std::unexpected(error);
    }
    return response;
}

std::expected<PreparedPipePassword, HRESULT> LogonSecretClient::prepare(
    PCWSTR sid,
    std::uint64_t request_id,
    std::uint32_t logon_session_id) const {
    auto request = Request{
        .operation = Operation::kPrepare,
        .request_id = request_id,
        .logon_session_id = logon_session_id,
    };
    if (!copy_fixed(sid, request.sid, std::size(request.sid))) {
        return std::unexpected(E_INVALIDARG);
    }
    auto response = transact(request);
    if (!response
        || response->password_length == 0
        || response->password_length >= std::size(response->password)
        || response->password[response->password_length] != L'\0') {
        if (response) {
            smile2unlock::logon_secret_ipc::clear_response(*response);
        }
        return std::unexpected(response ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) : response.error());
    }
    auto password = PreparedPipePassword{};
    std::copy_n(response->password, response->password_length + 1, password.password_.begin());
    password.size_ = response->password_length;
    smile2unlock::logon_secret_ipc::clear_response(*response);
    return password;
}

HRESULT LogonSecretClient::mark_stale(
    PCWSTR sid,
    std::uint64_t request_id,
    std::uint32_t logon_session_id) const {
    auto request = Request{
        .operation = Operation::kMarkStale,
        .request_id = request_id,
        .logon_session_id = logon_session_id,
    };
    if (!copy_fixed(sid, request.sid, std::size(request.sid))) {
        return E_INVALIDARG;
    }
    auto response = transact(request);
    if (!response) {
        return response.error();
    }
    smile2unlock::logon_secret_ipc::clear_response(*response);
    return S_OK;
}

HRESULT LogonSecretClient::store_for_current_user(
    std::span<const wchar_t> password) const {
    if (password.empty()
        || password.size() >= smile2unlock::logon_secret_ipc::kPasswordCapacity) {
        return E_INVALIDARG;
    }
    const auto sid = current_user_sid();
    if (!sid) {
        return sid.error();
    }
    auto request = Request{
        .operation = Operation::kStore,
        .request_id = GetTickCount64(),
    };
    if (request.request_id == 0) {
        request.request_id = 1;
    }
    if (!copy_fixed(sid->c_str(), request.sid, std::size(request.sid))) {
        return E_INVALIDARG;
    }
    std::copy(password.begin(), password.end(), request.password);
    request.password[password.size()] = L'\0';
    auto response = transact(request);
    if (!response) {
        return response.error();
    }
    smile2unlock::logon_secret_ipc::clear_response(*response);
    return S_OK;
}

HRESULT LogonSecretClient::clear_for_current_user() const {
    const auto sid = current_user_sid();
    if (!sid) {
        return sid.error();
    }
    auto request = Request{
        .operation = Operation::kClear,
        .request_id = GetTickCount64(),
    };
    if (request.request_id == 0) {
        request.request_id = 1;
    }
    if (!copy_fixed(sid->c_str(), request.sid, std::size(request.sid))) {
        return E_INVALIDARG;
    }
    auto response = transact(request);
    if (!response) {
        return response.error();
    }
    smile2unlock::logon_secret_ipc::clear_response(*response);
    return S_OK;
}
