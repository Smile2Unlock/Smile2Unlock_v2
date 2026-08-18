#include "../auth_service/logon_secret_protocol.h"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using smile2unlock::logon_secret_ipc::Operation;
using smile2unlock::logon_secret_ipc::Request;
using smile2unlock::logon_secret_ipc::Response;
using smile2unlock::logon_secret_ipc::Status;

void wipe_request(Request& request) noexcept {
    SecureZeroMemory(request.password, sizeof(request.password));
}

void wipe_response(Response& response) noexcept {
    SecureZeroMemory(response.password, sizeof(response.password));
    response.password_length = 0;
}

std::wstring current_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    const auto close_token = [&] { CloseHandle(token); };
    DWORD required = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    if (required == 0) {
        close_token();
        return {};
    }
    std::vector<std::byte> data(required);
    if (!GetTokenInformation(token, TokenUser, data.data(), required, &required)) {
        close_token();
        return {};
    }
    auto* user = reinterpret_cast<const TOKEN_USER*>(data.data());
    LPWSTR raw = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &raw)) {
        close_token();
        return {};
    }
    const std::wstring sid{raw};
    LocalFree(raw);
    close_token();
    return sid;
}

bool copy_field(std::wstring_view value, wchar_t* output, std::size_t capacity) {
    if (value.empty() || value.size() >= capacity) {
        return false;
    }
    std::copy(value.begin(), value.end(), output);
    output[value.size()] = L'\0';
    return true;
}

bool read_password(std::array<wchar_t, 513>& password) {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == INVALID_HANDLE_VALUE || input == nullptr) {
        return false;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(input, &mode)) {
        return false;
    }
    if (!SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT)) {
        return false;
    }
    std::wcout << L"Windows password: " << std::flush;
    DWORD read = 0;
    const auto ok = ReadConsoleW(input, password.data(), static_cast<DWORD>(password.size() - 1), &read, nullptr);
    (void)SetConsoleMode(input, mode);
    std::wcout << L"\n";
    if (!ok || read == 0) {
        return false;
    }
    while (read > 0 && (password[read - 1] == L'\r' || password[read - 1] == L'\n')) {
        --read;
    }
    password[read] = L'\0';
    return read != 0;
}

int run(Operation operation) {
    Request request{};
    request.operation = operation;
    request.request_id = GetTickCount64();
    if (request.request_id == 0) {
        request.request_id = 1;
    }
    const auto sid = current_sid();
    if (!copy_field(sid, request.sid, std::size(request.sid))) {
        std::wcerr << L"Unable to resolve the current Windows SID.\n";
        return 2;
    }
    std::array<wchar_t, 513> password{};
    if (operation == Operation::kStore && !read_password(password)) {
        std::wcerr << L"Password input failed. Run this command from an interactive console.\n";
        wipe_request(request);
        return 2;
    }
    if (operation == Operation::kStore) {
        if (!copy_field(password.data(), request.password, std::size(request.password))) {
            std::wcerr << L"Password is empty or too long.\n";
            wipe_request(request);
            SecureZeroMemory(password.data(), sizeof(password));
            return 2;
        }
    }

    Response response{};
    DWORD bytes_read = 0;
    const auto ok = CallNamedPipeW(
        smile2unlock::logon_secret_ipc::kPipeName,
        &request,
        sizeof(request),
        &response,
        sizeof(response),
        &bytes_read,
        5000);
    wipe_request(request);
    SecureZeroMemory(password.data(), sizeof(password));
    if (!ok || bytes_read != sizeof(response)
        || response.magic != smile2unlock::logon_secret_ipc::kMagic
        || response.version != smile2unlock::logon_secret_ipc::kVersion
        || response.request_id != request.request_id) {
        wipe_response(response);
        std::wcerr << L"Smile2Unlock auth service is unavailable or returned an invalid response.\n";
        return 3;
    }
    const auto status = response.status;
    wipe_response(response);
    if (status != Status::kOk) {
        std::wcerr << L"Auth service rejected the request (status "
                    << static_cast<unsigned>(status) << L").\n";
        return 4;
    }
    std::wcout << (operation == Operation::kStore
            ? L"Windows logon password stored.\n"
            : L"Windows logon password cleared.\n");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || (std::wstring_view{argv[1]} != L"--store"
        && std::wstring_view{argv[1]} != L"--clear")) {
        std::wcerr << L"Usage: su_password_tool.exe --store|--clear\n";
        return 64;
    }
    return run(std::wstring_view{argv[1]} == L"--store"
        ? Operation::kStore
        : Operation::kClear);
}
