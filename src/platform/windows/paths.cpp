#include "paths.h"

#include <windows.h>
#include <shlobj.h>

#include <memory>
#include <string>
#include <system_error>

namespace su::windows {

namespace {
[[noreturn]] void path_error(const char* operation, DWORD error) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}
}  // namespace

std::optional<std::filesystem::path> environment_path(std::wstring_view name) {
    const auto key = std::wstring{name};
    auto buffer = std::wstring(256, L'\0');
    while (true) {
        ::SetLastError(ERROR_SUCCESS);
        const auto length = ::GetEnvironmentVariableW(
            key.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            const auto error = ::GetLastError();
            if (error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND) {
                return std::nullopt;
            }
            path_error("GetEnvironmentVariableW", error);
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path{buffer};
        }
        // The required length includes the terminator. Retry if another
        // thread changes the variable between the two calls.
        buffer.resize(length);
    }
}

std::filesystem::path executable_path() {
    auto buffer = std::wstring(256, L'\0');
    while (true) {
        const auto length = ::GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            path_error("GetModuleFileNameW", ::GetLastError());
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path{buffer};
        }
        if (buffer.size() >= 32768) {
            path_error("GetModuleFileNameW", ERROR_INSUFFICIENT_BUFFER);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path roaming_app_data() {
    if (const auto directory = environment_path(L"APPDATA")) {
        return *directory;
    }
    if (const auto profile = environment_path(L"USERPROFILE")) {
        return *profile / L"AppData" / L"Roaming";
    }
    // Normal Windows accounts expose APPDATA. Use the Windows known folder
    // instead of a different temporary/working-directory config on failure.
    PWSTR raw = nullptr;
    const auto status = ::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw);
    const auto release = [](wchar_t* value) { ::CoTaskMemFree(value); };
    const auto directory = std::unique_ptr<wchar_t, decltype(release)>{raw, release};
    if (FAILED(status) || !directory) {
        path_error("SHGetKnownFolderPath(RoamingAppData)", static_cast<DWORD>(status));
    }
    return std::filesystem::path{directory.get()};
}

std::filesystem::path temporary_directory() {
    auto buffer = std::wstring(32768, L'\0');
    const auto length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0) path_error("GetTempPathW", ::GetLastError());
    if (length >= buffer.size()) path_error("GetTempPathW", ERROR_INSUFFICIENT_BUFFER);
    buffer.resize(length);
    return std::filesystem::path{buffer};
}

}  // namespace su::windows
