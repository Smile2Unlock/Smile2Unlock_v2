#include "profile_client.h"

#include "logon_secret_protocol.h"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace su::windows::profile_client {
namespace {

using smile2unlock::logon_secret_ipc::Operation;
using smile2unlock::logon_secret_ipc::Request;
using smile2unlock::logon_secret_ipc::Response;
using smile2unlock::logon_secret_ipc::Status;

std::expected<std::wstring, std::string> current_sid() {
    HANDLE token_raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_raw)) {
        return std::unexpected("failed to open the current process token");
    }
    const auto token = std::unique_ptr<void, decltype(&CloseHandle)>{token_raw, &CloseHandle};
    DWORD required = 0;
    (void)GetTokenInformation(token_raw, TokenUser, nullptr, 0, &required);
    if (required == 0) {
        return std::unexpected("failed to query the current user SID");
    }
    auto data = std::vector<std::byte>(required);
    if (!GetTokenInformation(token_raw, TokenUser, data.data(), required, &required)) {
        return std::unexpected("failed to read the current user SID");
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(data.data());
    LPWSTR raw_sid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &raw_sid)) {
        return std::unexpected("failed to format the current user SID");
    }
    const auto sid = std::wstring{raw_sid};
    LocalFree(raw_sid);
    return sid;
}

std::expected<std::wstring, std::string> utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return std::unexpected("value must not be empty");
    }
    const auto required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) {
        return std::unexpected("value is not valid UTF-8");
    }
    auto output = std::wstring(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
            output.data(), required) != required) {
        return std::unexpected("failed to convert UTF-8 input");
    }
    return output;
}

bool copy_wide(std::wstring_view source, wchar_t* destination, std::size_t capacity) {
    if (source.empty() || source.size() >= capacity) {
        return false;
    }
    std::ranges::copy(source, destination);
    destination[source.size()] = L'\0';
    return true;
}

std::string status_error(Status status) {
    switch (status) {
    case Status::kInvalidRequest: return "auth service rejected an invalid profile request";
    case Status::kAccessDenied: return "auth service denied profile access";
    case Status::kUnavailable: return "secure profile store is unavailable";
    case Status::kStaleOrConsumed: return "profile request is stale";
    case Status::kCorrupt: return "secure profile store is corrupt";
    case Status::kAuthenticationFailed: return "Windows password verification failed";
    case Status::kProfileNotFound: return "face profile was not found";
    case Status::kOk: return {};
    }
    return "auth service returned an unknown status";
}

std::expected<Response, std::string> transact(Request& request) {
    auto response = Response{};
    DWORD bytes_read = 0;
    const auto ok = CallNamedPipeW(
        smile2unlock::logon_secret_ipc::kPipeName,
        &request,
        sizeof(request),
        &response,
        sizeof(response),
        &bytes_read,
        5000);
    const auto request_id = request.request_id;
    smile2unlock::logon_secret_ipc::clear_request(request);
    if (!ok || bytes_read != sizeof(response)
        || response.magic != smile2unlock::logon_secret_ipc::kMagic
        || response.version != smile2unlock::logon_secret_ipc::kVersion
        || response.request_id != request_id) {
        smile2unlock::logon_secret_ipc::clear_response(response);
        return std::unexpected("Smile2Unlock auth service returned an invalid response");
    }
    if (response.status != Status::kOk) {
        const auto error = status_error(response.status);
        smile2unlock::logon_secret_ipc::clear_response(response);
        return std::unexpected(error);
    }
    return response;
}

std::expected<Request, std::string> request_for(Operation operation) {
    auto request = Request{};
    request.operation = operation;
    request.request_id = (GetTickCount64() << 16) ^ GetCurrentProcessId();
    if (request.request_id == 0) {
        request.request_id = 1;
    }
    const auto sid = current_sid();
    if (!sid || !copy_wide(*sid, request.sid, std::size(request.sid))) {
        smile2unlock::logon_secret_ipc::clear_request(request);
        return std::unexpected(sid ? "current SID is too long" : sid.error());
    }
    return request;
}

std::expected<std::string, std::string> response_payload(Response& response) {
    if (response.payload_length >= std::size(response.payload)
        || response.payload[response.payload_length] != 0) {
        smile2unlock::logon_secret_ipc::clear_response(response);
        return std::unexpected("auth service returned an invalid profile payload");
    }
    auto payload = std::string{
        reinterpret_cast<const char*>(response.payload), response.payload_length};
    smile2unlock::logon_secret_ipc::clear_response(response);
    return payload;
}

} // namespace

std::expected<std::string, std::string> list_profiles() {
    auto request = request_for(Operation::kListProfiles);
    if (!request) {
        return std::unexpected(request.error());
    }
    auto response = transact(*request);
    if (!response) {
        return std::unexpected(response.error());
    }
    return response_payload(*response);
}

std::expected<void, std::string> store_account_credential(
    std::string_view windows_password) {
    auto request = request_for(Operation::kStore);
    auto wide_password = utf8_to_wide(windows_password);
    if (!request || !wide_password
        || !copy_wide(*wide_password, request->password, std::size(request->password))) {
        if (request) smile2unlock::logon_secret_ipc::clear_request(*request);
        if (wide_password) {
            SecureZeroMemory(wide_password->data(), wide_password->size() * sizeof(wchar_t));
        }
        return std::unexpected("Windows password is invalid");
    }
    SecureZeroMemory(wide_password->data(), wide_password->size() * sizeof(wchar_t));
    auto response = transact(*request);
    if (!response) {
        return std::unexpected(response.error());
    }
    smile2unlock::logon_secret_ipc::clear_response(*response);
    return {};
}

std::expected<bool, std::string> account_credential_configured() {
    auto request = request_for(Operation::kCredentialStatus);
    if (!request) {
        return std::unexpected(request.error());
    }
    auto response = transact(*request);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->payload_length != 1 || response->payload[0] > 1) {
        smile2unlock::logon_secret_ipc::clear_response(*response);
        return std::unexpected("auth service returned an invalid credential status");
    }
    const auto configured = response->payload[0] == 1;
    smile2unlock::logon_secret_ipc::clear_response(*response);
    return configured;
}

std::expected<std::string, std::string> enroll_profile(
    std::string_view label,
    std::string_view embedding_source) {
    auto request = request_for(Operation::kEnrollProfile);
    const auto wide_label = utf8_to_wide(label);
    if (!request || !wide_label
        || !copy_wide(*wide_label, request->canonical_username, std::size(request->canonical_username))
        || embedding_source.empty()
        || embedding_source.size() >= std::size(request->payload)) {
        if (request) smile2unlock::logon_secret_ipc::clear_request(*request);
        return std::unexpected("profile label or embedding is invalid");
    }
    std::ranges::copy(embedding_source, request->payload);
    request->payload_length = static_cast<std::uint32_t>(embedding_source.size());
    auto response = transact(*request);
    if (!response) {
        return std::unexpected(response.error());
    }
    return response_payload(*response);
}

std::expected<bool, std::string> delete_profile(std::string_view profile_id) {
    auto request = request_for(Operation::kDeleteProfile);
    if (!request || profile_id.empty() || profile_id.size() >= std::size(request->payload)) {
        if (request) smile2unlock::logon_secret_ipc::clear_request(*request);
        return std::unexpected("profile id is invalid");
    }
    std::ranges::copy(profile_id, request->payload);
    request->payload_length = static_cast<std::uint32_t>(profile_id.size());
    auto response = transact(*request);
    if (!response) {
        return std::unexpected(response.error());
    }
    smile2unlock::logon_secret_ipc::clear_response(*response);
    return true;
}

std::expected<std::string, std::string> verify_profile(
    std::string_view embedding_source,
    bool liveness_ok) {
    auto request = request_for(Operation::kVerifyProfile);
    if (!request || embedding_source.empty()
        || embedding_source.size() >= std::size(request->payload)) {
        if (request) smile2unlock::logon_secret_ipc::clear_request(*request);
        return std::unexpected("face embedding is invalid");
    }
    std::ranges::copy(embedding_source, request->payload);
    request->payload_length = static_cast<std::uint32_t>(embedding_source.size());
    request->account_kind = liveness_ok ? 1U : 0U;
    auto response = transact(*request);
    if (!response) {
        return std::unexpected(response.error());
    }
    return response_payload(*response);
}

} // namespace su::windows::profile_client
