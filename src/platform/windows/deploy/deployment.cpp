// Plain (non-module) TU: winreg.h etc. are safe to include here.

#include "deployment.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsvc.h>
#include <shlobj.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace su::windeploy {

namespace {

constexpr wchar_t kClsid[] = L"{5fd3d285-0dd9-4362-8855-e0abaacd4af6}";
constexpr wchar_t kClsidKeyPath[] =
    L"SOFTWARE\\Classes\\CLSID\\{5fd3d285-0dd9-4362-8855-e0abaacd4af6}";
constexpr wchar_t kCredentialProvidersKeyPath[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\CredentialProviders";
constexpr wchar_t kCredentialProviderKeyPath[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\CredentialProviders\\"
    L"{5fd3d285-0dd9-4362-8855-e0abaacd4af6}";
constexpr wchar_t kServiceName[] = L"Smile2UnlockAuthService";

struct InstalledComponents {
    std::filesystem::path credential_provider;
    std::filesystem::path auth_service;
    std::filesystem::path recognition_agent;
};

std::expected<std::filesystem::path, std::string> program_files_root() {
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &raw))) {
        return std::unexpected("failed to resolve Program Files");
    }
    const auto path = std::filesystem::path{raw};
    ::CoTaskMemFree(raw);
    return path;
}

std::expected<void, std::string> copy_required_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    auto error = std::error_code{};
    if (!std::filesystem::is_regular_file(source, error)
        || std::filesystem::is_symlink(source, error)) {
        return std::unexpected("required deployment file is missing: " + source.string());
    }
    if (std::filesystem::is_regular_file(destination, error)
        && std::filesystem::equivalent(source, destination, error)) {
        return {};
    }
    error.clear();
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        return std::unexpected("failed to install " + source.filename().string()
            + ": " + error.message());
    }
    return {};
}

std::expected<InstalledComponents, std::string> stage_security_components(
    const std::filesystem::path& source_bin) {
    const auto program_files = program_files_root();
    if (!program_files) {
        return std::unexpected(program_files.error());
    }
    const auto install_root = *program_files / "Smile2Unlock";
    const auto install_bin = install_root / "bin";
    const auto install_models = install_root / "assets" / "models" / "seeta";
    const auto install_i18n = install_root / "assets" / "i18n";
    auto error = std::error_code{};
    std::filesystem::create_directories(install_bin, error);
    if (error) {
        return std::unexpected("failed to create the protected install directory: "
            + error.message());
    }
    std::filesystem::create_directories(install_models, error);
    if (error) {
        return std::unexpected("failed to create the protected model directory: "
            + error.message());
    }
    std::filesystem::create_directories(install_i18n, error);
    if (error) {
        return std::unexpected("failed to create the translation directory: "
            + error.message());
    }

    const auto source_service = std::filesystem::is_regular_file(
            source_bin / "Smile2UnlockAuthService.exe", error)
        ? source_bin / "Smile2UnlockAuthService.exe"
        : source_bin / "su_auth_service.exe";
    const auto installed = InstalledComponents{
        .credential_provider = install_bin / "su_credential_provider.dll",
        .auth_service = install_bin / "Smile2UnlockAuthService.exe",
        .recognition_agent = install_bin / "su_recognition_agent.exe",
    };
    for (const auto& pair : {
             std::pair{source_service, installed.auth_service},
             std::pair{source_bin / "su_recognition_agent.exe", installed.recognition_agent},
             std::pair{source_bin / "su_app.exe", install_bin / "su_app.exe"},
             std::pair{source_bin / "su_deploy_helper.exe", install_bin / "su_deploy_helper.exe"},
             std::pair{source_bin / "su_password_tool.exe", install_bin / "su_password_tool.exe"},
             std::pair{source_bin / "su_credential_provider.dll", install_bin / "su_credential_provider.dll"},
             std::pair{source_bin / "Smile2Unlock.ico", install_bin / "Smile2Unlock.ico"},
         }) {
        if (const auto copied = copy_required_file(pair.first, pair.second); !copied) {
            return std::unexpected(copied.error());
        }
    }

    // The agent is dynamically linked to SeetaFace and the MinGW runtime.
    // Copy only the known runtime set; unrelated DLLs beside the package are
    // never promoted into the trusted install directory.
    constexpr auto runtime_names = std::array{
        L"libgcc_s_seh-1.dll", L"libstdc++-6.dll", L"libwinpthread-1.dll",
        L"libgomp-1.dll", L"libSeetaFaceAntiSpoofingX600.dll",
        L"libSeetaFaceDetector600.dll", L"libSeetaFaceLandmarker600.dll",
        L"libSeetaFaceRecognizer610.dll", L"libSeetaAuthorize.dll", L"libtennis.dll",
    };
    for (const auto* name : runtime_names) {
        const auto source = source_bin / name;
        if (const auto copied = copy_required_file(source, install_bin / name); !copied) {
            return std::unexpected(copied.error());
        }
    }

    const auto source_assets = std::filesystem::is_directory(source_bin / "assets", error)
        ? source_bin / "assets" / "models" / "seeta"
        : source_bin.parent_path() / "assets" / "models" / "seeta";
    constexpr auto model_names = std::array{
        L"face_detector.csta", L"face_landmarker_pts5.csta", L"face_recognizer.csta",
        L"fas_first.csta", L"fas_second.csta",
    };
    for (const auto* name : model_names) {
        if (const auto copied = copy_required_file(
                source_assets / name, install_models / name); !copied) {
            return std::unexpected(copied.error());
        }
    }
    for (const auto* name : {L"en.json", L"zh-CN.json"}) {
        if (const auto copied = copy_required_file(
                source_assets.parent_path().parent_path() / "i18n" / name,
                install_i18n / name); !copied) {
            return std::unexpected(copied.error());
        }
    }
    return installed;
}

std::expected<std::filesystem::path, std::string> stage_credential_provider(
    const std::filesystem::path& source_dll) {
    const auto program_files = program_files_root();
    if (!program_files) {
        return std::unexpected(program_files.error());
    }
    const auto install_bin = *program_files / "Smile2Unlock" / "bin";
    auto error = std::error_code{};
    std::filesystem::create_directories(install_bin, error);
    if (error) {
        return std::unexpected("failed to create the protected install directory: "
            + error.message());
    }
    // LogonUI can keep the previous provider DLL mapped for the entire login
    // session. Publish immutable, content-addressed filenames so an update
    // never needs to overwrite a loaded image; the registry switch is atomic
    // from the next LogonUI activation onward.
    std::ifstream input(source_dll, std::ios::binary);
    if (!input) {
        return std::unexpected("required deployment file is missing: " + source_dll.string());
    }
    auto hash = std::uint64_t{1469598103934665603ULL};
    std::array<char, 64 * 1024> buffer{};
    while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()))
        || input.gcount() != 0) {
        for (std::streamsize index = 0; index < input.gcount(); ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= 1099511628211ULL;
        }
    }
    if (input.bad()) {
        return std::unexpected("failed to read the credential provider: " + source_dll.string());
    }
    const auto destination = install_bin
        / std::format("su_credential_provider_{:016x}.dll", hash);
    if (std::filesystem::is_regular_file(destination, error)) {
        return destination;
    }
    if (const auto copied = copy_required_file(source_dll, destination); !copied) {
        return std::unexpected(copied.error());
    }
    return destination;
}

std::filesystem::path current_binary_directory() {
    auto path = std::wstring(32768, L'\0');
    const auto length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

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

std::string json_escape(std::string_view text) {
    auto escaped = std::string{};
    escaped.reserve(text.size());
    for (const auto ch : text) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                escaped += std::format("\\u{:04x}", static_cast<unsigned char>(ch));
            } else {
                escaped += ch;
            }
        }
    }
    return escaped;
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
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCredentialProviderKeyPath, 0,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD disabled = 0;
    DWORD type = 0;
    DWORD size = sizeof(disabled);
    const auto status = ::RegQueryValueExW(
        key, L"Disabled", nullptr, &type, reinterpret_cast<BYTE*>(&disabled), &size);
    ::RegCloseKey(key);
    return status == ERROR_FILE_NOT_FOUND
        || (status == ERROR_SUCCESS && type == REG_DWORD && disabled == 0);
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
    auto service_path_managed = false;
    if (const auto program_files = program_files_root(); program_files && !service_binary.empty()) {
        auto configured_path = std::filesystem::path{utf8_to_wide(service_binary)};
        auto configured_text = configured_path.wstring();
        if (configured_text.size() >= 2 && configured_text.front() == L'"'
            && configured_text.back() == L'"') {
            configured_path = configured_text.substr(1, configured_text.size() - 2);
        }
        const auto expected_path = *program_files / "Smile2Unlock" / "bin"
            / "Smile2UnlockAuthService.exe";
        auto error = std::error_code{};
        service_path_managed = std::filesystem::equivalent(
            configured_path, expected_path, error) && !error;
    }
    const auto service_ready = service_running && service_path_managed;
    snapshot.targets.push_back(ComponentStatus{
        .id = std::string(kAuthServiceId),
        .service = "Smile2Unlock Auth Service",
        .effective_path = service_binary,
        .role = "login-and-lock",
        .state = auth_service_installed()
            ? (!service_path_managed ? "conflict"
                : (service_ready ? "managed" : "supported"))
            : "absent",
        .detail = !auth_service_installed()
            ? "auth service is not installed"
            : (!service_path_managed ? "service uses a legacy or unmanaged binary path"
                : (service_running ? "service is running"
                                   : "service is installed but not running")),
        .configured = service_ready,
        .configurable = true,
        .managed = service_ready,
    });
    return snapshot;
}

std::expected<void, std::string> register_credential_provider(const std::string& dll_path) {
    const auto source_dll = std::filesystem::path{utf8_to_wide(dll_path)};
    const auto installed_dll = stage_credential_provider(source_dll);
    if (!installed_dll) {
        return std::unexpected(installed_dll.error());
    }
    const auto wide_dll = installed_dll->wstring();
    if (wide_dll.empty() || !file_exists(wide_dll)) {
        return std::unexpected("credential provider DLL not found: " + dll_path);
    }

    HKEY clsid_key = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kClsidKeyPath, 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &clsid_key, nullptr) != ERROR_SUCCESS) {
        return std::unexpected("failed to open the CLSID registry key");
    }
    const auto clsid_name_status = ::RegSetValueExW(clsid_key, L"", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(L"Smile2Unlock"), sizeof(L"Smile2Unlock"));
    ::RegCloseKey(clsid_key);
    if (clsid_name_status != ERROR_SUCCESS) {
        return std::unexpected("failed to write the CLSID display name");
    }

    const auto inproc_path = std::wstring(kClsidKeyPath) + L"\\InprocServer32";
    HKEY inproc_key = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, inproc_path.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &inproc_key, nullptr) != ERROR_SUCCESS) {
        return std::unexpected("failed to open the InprocServer32 registry key");
    }
    const auto dll_bytes = (wide_dll.size() + 1) * sizeof(wchar_t);
    const auto dll_status = ::RegSetValueExW(inproc_key, L"", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(wide_dll.c_str()), static_cast<DWORD>(dll_bytes));
    constexpr wchar_t kApartment[] = L"Apartment";
    const auto threading_status = ::RegSetValueExW(inproc_key, L"ThreadingModel", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(kApartment), sizeof(kApartment));
    ::RegCloseKey(inproc_key);
    if (dll_status != ERROR_SUCCESS || threading_status != ERROR_SUCCESS) {
        return std::unexpected("failed to write the InprocServer32 registration");
    }

    // LogonUI discovers credential providers by CLSID-named subkeys.
    HKEY cp_key = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kCredentialProviderKeyPath, 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &cp_key, nullptr) != ERROR_SUCCESS) {
        return std::unexpected("failed to open the CredentialProviders registry key");
    }
    constexpr wchar_t kProviderName[] = L"Smile2Unlock Credential Provider";
    const auto enrollment_status = ::RegSetValueExW(cp_key, L"", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(kProviderName), sizeof(kProviderName));
    const auto enabled_status = ::RegDeleteValueW(cp_key, L"Disabled");
    ::RegCloseKey(cp_key);
    if (enrollment_status != ERROR_SUCCESS
        || (enabled_status != ERROR_SUCCESS && enabled_status != ERROR_FILE_NOT_FOUND)) {
        return std::unexpected("failed to enroll the credential provider");
    }
    return {};
}

std::expected<void, std::string> unregister_credential_provider() {
    const auto removed = ::RegDeleteTreeW(HKEY_LOCAL_MACHINE, kCredentialProviderKeyPath);
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
        return std::unexpected("failed to remove the CredentialProviders registration");
    }
    // Remove values written by early development builds, which incorrectly
    // stored the CLSID directly below the parent key.
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
    // Keep the CLSID/InprocServer32 registration for diagnostics.
    return {};
}

std::expected<void, std::string> ensure_auth_service() {
    const auto manager = ::OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr) {
        return std::unexpected("failed to open the service control manager");
    }
    auto service = ::OpenServiceW(
        manager,
        kServiceName,
        SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
    if (service != nullptr) {
        SERVICE_STATUS current{};
        if (::QueryServiceStatus(service, &current)
            && current.dwCurrentState != SERVICE_STOPPED) {
            (void)::ControlService(service, SERVICE_CONTROL_STOP, &current);
            for (int attempt = 0; attempt < 50; ++attempt) {
                ::Sleep(100);
                if (::QueryServiceStatus(service, &current)
                    && current.dwCurrentState == SERVICE_STOPPED) {
                    break;
                }
            }
            if (current.dwCurrentState != SERVICE_STOPPED) {
                ::CloseServiceHandle(service);
                ::CloseServiceHandle(manager);
                return std::unexpected("timed out while stopping the auth service for update");
            }
        }
    }
    const auto installed = stage_security_components(current_binary_directory());
    if (!installed) {
        if (service != nullptr) {
            (void)::StartServiceW(service, 0, nullptr);
            ::CloseServiceHandle(service);
        }
        ::CloseServiceHandle(manager);
        return std::unexpected(installed.error());
    }
    if (service == nullptr) {
        if (::GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
            ::CloseServiceHandle(manager);
            return std::unexpected("failed to open the auth service");
        }
        const auto service_binary = installed->auth_service.wstring();
        if (service_binary.empty() || !file_exists(service_binary)) {
            ::CloseServiceHandle(manager);
            return std::unexpected("auth service binary is not deployed next to the helper");
        }
        const auto quoted_binary = L"\"" + service_binary + L"\"";
        service = ::CreateServiceW(
            manager,
            kServiceName,
            L"Smile2Unlock Authentication Service",
            SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            quoted_binary.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        if (service == nullptr) {
            ::CloseServiceHandle(manager);
            return std::unexpected("failed to register the auth service");
        }
    }
    const auto quoted_binary = L"\"" + installed->auth_service.wstring() + L"\"";
    if (!::ChangeServiceConfigW(
            service,
            SERVICE_NO_CHANGE,
            SERVICE_AUTO_START,
            SERVICE_NO_CHANGE,
            quoted_binary.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr)) {
        ::CloseServiceHandle(service);
        ::CloseServiceHandle(manager);
        return std::unexpected("failed to move the auth service to Program Files");
    }
    const auto started = ::StartServiceW(service, 0, nullptr);
    const auto start_error = started ? ERROR_SUCCESS : ::GetLastError();
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    if (!started && start_error != ERROR_SERVICE_ALREADY_RUNNING) {
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
            json_escape(target.id), json_escape(target.service), json_escape(target.effective_path),
            json_escape(target.role), json_escape(target.state), json_escape(target.detail),
            target.configured ? "true" : "false",
            target.configurable ? "true" : "false",
            target.managed ? "true" : "false");
    }
    json += "]";
    return json;
}

}  // namespace su::windeploy
