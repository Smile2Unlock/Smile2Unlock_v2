// Plain (non-module) TU: winreg.h etc. are safe to include here.

#include "deployment.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>

#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace su::windeploy {

namespace {

constexpr wchar_t kClsid[] = L"{5fd3d285-0dd9-4362-8855-e0abaacd4af6}";
constexpr wchar_t kClsidKeyPath[] =
    L"SOFTWARE\\Classes\\CLSID\\{5fd3d285-0dd9-4362-8855-e0abaacd4af6}";
constexpr wchar_t kCredentialProvidersKeyPath[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\CredentialProviders";
constexpr wchar_t kServiceName[] = L"Smile2UnlockAuthService";

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    auto wide = std::wstring(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    auto narrow = std::string(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), length,
        nullptr, nullptr);
    return narrow;
}

std::optional<std::wstring> reg_read_string(HKEY root, const wchar_t* path, const wchar_t* name) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::wstring buffer(512, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    const auto status = ::RegQueryValueExW(key, name, nullptr, nullptr,
        reinterpret_cast<BYTE*>(buffer.data()), &size);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return std::nullopt;
    }
    buffer.resize(size / sizeof(wchar_t) - (buffer[size / sizeof(wchar_t) - 1] == L'\0' ? 1 : 0));
    return buffer;
}

bool file_exists(const std::wstring& path) {
    const auto attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace

bool process_elevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const auto queried = ::GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &size);
    ::CloseHandle(token);
    return queried && elevation.TokenIsElevated != 0;
}

bool credential_provider_enrolled() {
    // Any numeric value under CredentialProviders equal to the CLSID.
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCredentialProvidersKeyPath, 0,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return false;
    }
    auto enrolled = false;
    for (DWORD index = 0;; ++index) {
        wchar_t name[64] = {};
        DWORD name_size = 64;
        BYTE value[256] = {};
        DWORD value_size = sizeof(value);
        const auto status = ::RegEnumValueW(
            key, index, name, &name_size, nullptr, nullptr, value, &value_size);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            continue;
        }
        std::wstring text(reinterpret_cast<wchar_t*>(value),
            value_size / sizeof(wchar_t));
        while (!text.empty() && text.back() == L'\0') {
            text.pop_back();
        }
        if (text == kClsid) {
            enrolled = true;
            break;
        }
    }
    ::RegCloseKey(key);
    return enrolled;
}

bool credential_provider_registered() {
    const auto dll = credential_provider_dll_path();
    return !dll.empty() && file_exists(utf8_to_wide(dll));
}

std::string credential_provider_dll_path() {
    const auto inproc = std::wstring(kClsidKeyPath) + L"\\InprocServer32";
    const auto dll = reg_read_string(HKEY_LOCAL_MACHINE, inproc.c_str(), L"");
    return dll ? wide_to_utf8(*dll) : std::string{};
}

bool auth_service_installed() {
    const auto manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return false;
    }
    const auto service = ::OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    if (service != nullptr) {
        ::CloseServiceHandle(service);
    }
    ::CloseServiceHandle(manager);
    return service != nullptr;
}

bool auth_service_running() {
    const auto manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return false;
    }
    const auto service = ::OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        ::CloseServiceHandle(manager);
        return false;
    }
    SERVICE_STATUS status{};
    const auto queried = ::QueryServiceStatus(service, &status);
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    return queried && status.dwCurrentState == SERVICE_RUNNING;
}

std::string auth_service_binary_path() {
    const auto manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return {};
    }
    const auto service = ::OpenServiceW(manager, kServiceName, SERVICE_QUERY_CONFIG);
    if (service == nullptr) {
        ::CloseServiceHandle(manager);
        return {};
    }
    DWORD needed = 0;
    (void)::QueryServiceConfigW(service, nullptr, 0, &needed);
    auto buffer = std::vector<BYTE>(needed);
    LPQUERY_SERVICE_CONFIGW config = nullptr;
    if (needed > 0) {
        config = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
        (void)::QueryServiceConfigW(service, config, needed, &needed);
    }
    std::string path;
    if (config != nullptr && config->lpBinaryPathName != nullptr) {
        path = wide_to_utf8(config->lpBinaryPathName);
    }
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    return path;
}

std::expected<DeploymentSnapshot, std::string> inspect_deployment() {
    auto snapshot = DeploymentSnapshot{};
    const auto dll = credential_provider_dll_path();
    const auto enrolled = credential_provider_enrolled();
    snapshot.targets.push_back(ComponentStatus{
        .id = std::string(kCredentialProviderId),
        .service = "Smile2Unlock Credential Provider",
        .effective_path = dll,
        .role = "login-and-lock",
        .state = enrolled ? "managed" : (dll.empty() ? "absent" : "supported"),
        .detail = dll.empty()
            ? "credential provider DLL is not registered"
            : (enrolled ? "enrolled in the logon UI"
                        : "CLSID registered but not enrolled in the logon UI"),
        .configured = enrolled,
        .configurable = true,
        .managed = enrolled,
    });

    const auto service_binary = auth_service_binary_path();
    const auto service_running = auth_service_running();
    snapshot.targets.push_back(ComponentStatus{
        .id = std::string(kAuthServiceId),
        .service = "Smile2Unlock Auth Service",
        .effective_path = service_binary,
        .role = "login-and-lock",
        .state = auth_service_installed()
            ? (service_running ? "managed" : "supported")
            : "absent",
        .detail = !auth_service_installed()
            ? "auth service is not installed"
            : (service_running ? "service is running"
                               : "service is installed but not running"),
        .configured = service_running,
        .configurable = true,
        .managed = service_running,
    });
    return snapshot;
}

std::expected<void, std::string> register_credential_provider(const std::string& dll_path) {
    const auto wide_dll = utf8_to_wide(dll_path);
    if (wide_dll.empty() || !file_exists(wide_dll)) {
        return std::unexpected("credential provider DLL not found: " + dll_path);
    }

    HKEY clsid_key = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kClsidKeyPath, 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &clsid_key, nullptr) != ERROR_SUCCESS) {
        return std::unexpected("failed to open the CLSID registry key");
    }
    ::RegSetValueExW(clsid_key, L"", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(L"Smile2Unlock"), sizeof(L"Smile2Unlock"));
    ::RegCloseKey(clsid_key);

    const auto inproc_path = std::wstring(kClsidKeyPath) + L"\\InprocServer32";
    HKEY inproc_key = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, inproc_path.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &inproc_key, nullptr) != ERROR_SUCCESS) {
        return std::unexpected("failed to open the InprocServer32 registry key");
    }
    const auto dll_bytes = (wide_dll.size() + 1) * sizeof(wchar_t);
    ::RegSetValueExW(inproc_key, L"", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(wide_dll.c_str()), static_cast<DWORD>(dll_bytes));
    constexpr wchar_t kApartment[] = L"Apartment";
    ::RegSetValueExW(inproc_key, L"ThreadingModel", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(kApartment), sizeof(kApartment));
    ::RegCloseKey(inproc_key);

    // Enroll in the logon UI under the next free index (1, 2, ...).
    HKEY cp_key = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kCredentialProvidersKeyPath, 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &cp_key, nullptr) != ERROR_SUCCESS) {
        return std::unexpected("failed to open the CredentialProviders registry key");
    }
    if (!credential_provider_enrolled()) {
        DWORD index = 1;
        wchar_t name[16] = {};
        while (true) {
            std::swprintf(name, 16, L"%lu", index);
            DWORD type = 0;
            const auto status = ::RegQueryValueExW(
                cp_key, name, nullptr, &type, nullptr, nullptr);
            if (status != ERROR_SUCCESS) {
                break;
            }
            ++index;
        }
        ::RegSetValueExW(cp_key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(kClsid), sizeof(kClsid));
    }
    ::RegCloseKey(cp_key);
    return {};
}

std::expected<void, std::string> unregister_credential_provider() {
    HKEY cp_key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCredentialProvidersKeyPath, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &cp_key) == ERROR_SUCCESS) {
        for (DWORD index = 0;; ++index) {
            wchar_t name[64] = {};
            DWORD name_size = 64;
            BYTE value[256] = {};
            DWORD value_size = sizeof(value);
            const auto status = ::RegEnumValueW(
                cp_key, index, name, &name_size, nullptr, nullptr, value, &value_size);
            if (status == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (status != ERROR_SUCCESS) {
                continue;
            }
            std::wstring text(reinterpret_cast<wchar_t*>(value),
                value_size / sizeof(wchar_t));
            while (!text.empty() && text.back() == L'\0') {
                text.pop_back();
            }
            if (text == kClsid) {
                ::RegDeleteValueW(cp_key, name);
                break;
            }
        }
        ::RegCloseKey(cp_key);
    }
    // Keep the CLSID registration so the DLL can still be probed; the
    // logon-UI enrollment is what is removed.
    return {};
}

std::expected<void, std::string> ensure_auth_service() {
    if (auth_service_running()) {
        return {};
    }
    // SC_MANAGER_START_SERVICE (0x40) is missing from mingw's winsvc.h.
    constexpr auto kManagerStartService = 0x40u;
    const auto manager = ::OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT | kManagerStartService);
    if (manager == nullptr) {
        return std::unexpected("failed to open the service control manager");
    }
    const auto service = ::OpenServiceW(manager, kServiceName, SERVICE_START);
    if (service == nullptr) {
        ::CloseServiceHandle(manager);
        return std::unexpected("auth service is not installed");
    }
    const auto started = ::StartServiceW(service, 0, nullptr);
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    if (!started && ::GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        return std::unexpected("failed to start the auth service");
    }
    return {};
}

std::string snapshot_json(const DeploymentSnapshot& snapshot) {
    std::string json = "[";
    for (std::size_t i = 0; i < snapshot.targets.size(); ++i) {
        const auto& target = snapshot.targets[i];
        if (i > 0) {
            json += ',';
        }
        json += std::format(
            R"({{"id":"{}","service":"{}","effective_path":"{}","role":"{}","state":"{}","detail":"{}","configured":{},"configurable":{},"managed":{},"wallet_available":false,"wallet_enabled":false}})",
            target.id, target.service, target.effective_path, target.role, target.state,
            target.detail,
            target.configured ? "true" : "false",
            target.configurable ? "true" : "false",
            target.managed ? "true" : "false");
    }
    json += "]";
    return json;
}

}  // namespace su::windeploy
