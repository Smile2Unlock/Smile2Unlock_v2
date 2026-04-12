module;

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfidl.h>
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <secext.h>
#include <GL/gl.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "utils/logger.h"
#include "inicpp.hpp"

export module smile2unlock.application;

import std;
import app_paths;
import smile2unlock.models;
import smile2unlock.service;

export namespace smile2unlock {

struct CameraDeviceOption {
    int index{};
    std::string label;
};

struct VersionCheckResult {
    bool success{};
    bool update_available{};
    bool manual{};
    std::string current_version;
    std::string remote_version;
    std::string release_url;
    std::string published_at;
    std::string error_message;
};

class Application {
public:
    Application();
    ~Application();

    bool Initialize();
    void Run();
    void Shutdown();
    
    // 设置后端服务（本地或远程）
    void SetBackend(std::unique_ptr<IBackendService> backend) {
        backend_ = std::move(backend);
    }
    
    // 是否使用远程后端
    bool IsUsingRemoteBackend() const {
        return is_remote_backend_;
    }
    
    void SetRemoteBackendFlag(bool is_remote) {
        is_remote_backend_ = is_remote;
    }

private:
    void RenderUI();
    void RenderShell();
    void RenderTitleBar();
    void RenderSidebar();
    void RenderOverview();
    void RenderInjectorPanel();
    void RenderUsersPanel();
    void RenderRecognizerPanel();
    void RenderConfigPanel();
    void RefreshCameraDeviceList();
    void SyncCameraPreviewState();
    void PollPreviewStream();
    void RefreshFacePreview();
    void UpdatePreviewTexture(const std::vector<unsigned char>& image_data, int width, int height);
    void ClearPreviewTexture();
    void ApplyNordStyle();
    void RenderSectionHeader(const char* eyebrow, const char* title, const char* body = nullptr);
    void RenderInfoRow(const char* label, const std::string& value, const ImVec4& value_color = ImVec4(0, 0, 0, 0));
    void RenderStatTile(const char* id, const char* label, const std::string& value, const std::string& hint, const ImVec4& accent, float width);
    bool RenderNavItem(const char* id, const char* label, const char* hint, bool selected);
    bool RenderWindowControlButton(int icon, const ImVec4& button_color, const ImVec4& hover_color, float width = 68.0f);
    void RenderStatusMessage();
    void BeginPanelCard(const char* id, float height = 0.0f);
    void EndPanelCard();
    const char* Tr(std::string_view key) const;
    bool ApplyLanguage(std::string_view requested_language);
    std::string ResolveInitialLanguage(std::string_view configured_language) const;
    std::unordered_map<std::string, std::string> LoadLanguageFile(const std::string& language_code, std::string& error_message) const;
    static bool IsEnglishSystemUiLanguage();
    float CalcButtonWidth(std::string_view label, float min_width) const;
    bool CanFitInline(std::initializer_list<float> widths, float spacing = 12.0f) const;
    void LoadRecognizerConfigOnce();
    void StartVersionCheck(bool manual = false);
    void PollVersionCheck();
    void RenderVersionPanel();
    void OpenLatestReleasePage() const;

    std::unique_ptr<IBackendService> backend_;
    GLFWwindow* window_;

    struct UIState {
        bool show_demo_window{};
        bool injector_tab_active{};
        char new_username[64]{};
        char new_password[64]{};
        char new_remark[256]{};
        bool show_edit_user{};
        int editing_user_id{};
        char edit_username[64]{};
        char edit_password[64]{};
        char edit_remark[256]{};
        std::string status_message;
        float status_message_timer{};
        int selected_user_id{};
        bool show_face_capture{};
        char face_remark[256]{};
        bool save_face_success{};
        GLuint preview_texture{};
        int preview_width{};
        int preview_height{};
        bool has_preview{};
        bool request_preview_refresh{};
        bool camera_enabled{};
        std::string preview_message;
        int active_panel{};
        FaceRecognizerConfig recognizer_config{};
        FaceRecognizerConfig recognizer_saved_config{};
        bool recognizer_config_loaded{};
        bool recognizer_config_dirty{};
        std::string recognizer_config_message;
        std::vector<CameraDeviceOption> camera_devices;
        bool camera_devices_loaded{};
        std::string camera_devices_message;
        VersionCheckResult version_check{};
        std::future<VersionCheckResult> version_check_future;
        bool version_check_in_progress{};
        bool version_check_started{};
        bool version_update_prompt_open{};
        std::string version_last_checked_at;
        
        // 缓存的 DLL 状态
        DllStatus cached_dll_status;
        float dll_status_refresh_timer{-1.0f};  // -1 表示需要立即刷新
    };

    UIState ui_state_;
    std::string imgui_ini_path_;
    bool initialized_;
    bool is_remote_backend_{false};
    std::string active_language_code_{"zh-CN"};
    std::unordered_map<std::string, std::string> active_translations_;
    std::unordered_map<std::string, std::string> fallback_translations_;
};

} // namespace smile2unlock

module :private;

namespace smile2unlock {

namespace NordColors {
    constexpr ImVec4 Nord0 = ImVec4(46.0f / 255.0f, 52.0f / 255.0f, 64.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord1 = ImVec4(59.0f / 255.0f, 66.0f / 255.0f, 82.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord2 = ImVec4(67.0f / 255.0f, 76.0f / 255.0f, 94.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord3 = ImVec4(76.0f / 255.0f, 86.0f / 255.0f, 106.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord4 = ImVec4(216.0f / 255.0f, 222.0f / 255.0f, 233.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord5 = ImVec4(229.0f / 255.0f, 233.0f / 255.0f, 240.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord6 = ImVec4(236.0f / 255.0f, 239.0f / 255.0f, 244.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord7 = ImVec4(143.0f / 255.0f, 188.0f / 255.0f, 187.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord8 = ImVec4(136.0f / 255.0f, 192.0f / 255.0f, 208.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord9 = ImVec4(129.0f / 255.0f, 161.0f / 255.0f, 193.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord10 = ImVec4(94.0f / 255.0f, 129.0f / 255.0f, 172.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord11 = ImVec4(191.0f / 255.0f, 97.0f / 255.0f, 106.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord12 = ImVec4(208.0f / 255.0f, 135.0f / 255.0f, 112.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord13 = ImVec4(235.0f / 255.0f, 203.0f / 255.0f, 139.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord14 = ImVec4(163.0f / 255.0f, 190.0f / 255.0f, 140.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord15 = ImVec4(180.0f / 255.0f, 142.0f / 255.0f, 173.0f / 255.0f, 1.0f);
}

using namespace NordColors;

enum PanelIndex {
    PanelInjector = 0,
    PanelUsers = 1,
    PanelRecognizer = 2,
    PanelConfig = 3
};

enum WindowControlIcon {
    WindowControlMinimize = 0,
    WindowControlMaximize = 1,
    WindowControlRestore = 2,
    WindowControlClose = 3
};

enum class UiLanguage {
    ZhCN,
    EnUS
};

constexpr GLint kGlClampToEdge = 0x812F;
constexpr ImGuiWindowFlags kStaticChildFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
constexpr std::string_view kCurrentVersion = SMILE2UNLOCK_VERSION;
constexpr std::string_view kLatestReleaseApiUrl = SMILE2UNLOCK_RELEASES_API;
constexpr std::string_view kReleasesPageUrl = SMILE2UNLOCK_RELEASES_PAGE;

std::wstring Utf8ToWide(std::string_view input) {
    if (input.empty()) {
        return {};
    }
    const int required_size = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (required_size <= 0) {
        return {};
    }
    std::wstring output(static_cast<size_t>(required_size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), required_size);
    return output;
}

std::string WideToUtf8(std::wstring_view input) {
    if (input.empty()) {
        return {};
    }
    const int required_size = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (required_size <= 0) {
        return {};
    }
    std::string output(static_cast<size_t>(required_size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), required_size, nullptr, nullptr);
    return output;
}

std::string TrimCopy(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string NormalizeVersion(std::string version) {
    version = TrimCopy(std::move(version));
    if (!version.empty() && (version.front() == 'v' || version.front() == 'V')) {
        version.erase(version.begin());
    }
    return version;
}

std::vector<int> ParseVersionParts(std::string version) {
    version = NormalizeVersion(std::move(version));
    std::vector<int> parts;
    std::string current;
    for (const char ch : version) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            current.push_back(ch);
        } else {
            if (!current.empty()) {
                parts.push_back(std::stoi(current));
                current.clear();
            }
            if (ch != '.') {
                break;
            }
        }
    }
    if (!current.empty()) {
        parts.push_back(std::stoi(current));
    }
    return parts;
}

int CompareVersions(std::string lhs, std::string rhs) {
    const auto left_parts = ParseVersionParts(std::move(lhs));
    const auto right_parts = ParseVersionParts(std::move(rhs));
    const size_t count = std::max(left_parts.size(), right_parts.size());
    for (size_t index = 0; index < count; ++index) {
        const int left = index < left_parts.size() ? left_parts[index] : 0;
        const int right = index < right_parts.size() ? right_parts[index] : 0;
        if (left < right) {
            return -1;
        }
        if (left > right) {
            return 1;
        }
    }
    return 0;
}

std::optional<std::string> ExtractJsonStringField(const std::string& json, const std::string& field_name) {
    const std::regex pattern("\"" + field_name + "\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"");
    std::smatch match;
    if (!std::regex_search(json, match, pattern) || match.size() < 2) {
        return std::nullopt;
    }

    std::string value = match[1].str();
    value = std::regex_replace(value, std::regex(R"(\\/)"), "/");
    value = std::regex_replace(value, std::regex(R"(\\")"), "\"");
    value = std::regex_replace(value, std::regex(R"(\\r)"), "\r");
    value = std::regex_replace(value, std::regex(R"(\\n)"), "\n");
    value = std::regex_replace(value, std::regex(R"(\\t)"), "\t");
    value = std::regex_replace(value, std::regex(R"(\\\\)"), "\\");
    return value;
}

std::string FormatIsoTimestamp(std::string timestamp) {
    std::replace(timestamp.begin(), timestamp.end(), 'T', ' ');
    if (!timestamp.empty() && timestamp.back() == 'Z') {
        timestamp.pop_back();
        timestamp += " UTC";
    }
    return timestamp;
}

std::string HttpGetUtf8(std::string_view url) {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    std::wstring wide_url = Utf8ToWide(url);
    if (wide_url.empty() || !WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
        throw std::runtime_error("Failed to parse update URL.");
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    const DWORD request_flags = secure ? WINHTTP_FLAG_SECURE : 0;

    auto close_handle = [](HINTERNET handle) {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    };

    std::unique_ptr<void, decltype(close_handle)> session(
        WinHttpOpen(L"Smile2Unlock/UpdateChecker", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
        close_handle);
    if (!session) {
        throw std::runtime_error("Failed to open WinHTTP session.");
    }

    std::unique_ptr<void, decltype(close_handle)> connection(
        WinHttpConnect(static_cast<HINTERNET>(session.get()), host.c_str(), components.nPort, 0),
        close_handle);
    if (!connection) {
        throw std::runtime_error("Failed to connect to update server.");
    }

    std::unique_ptr<void, decltype(close_handle)> request(
        WinHttpOpenRequest(static_cast<HINTERNET>(connection.get()), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, request_flags),
        close_handle);
    if (!request) {
        throw std::runtime_error("Failed to create update request.");
    }

    constexpr wchar_t kHeaders[] =
        L"User-Agent: Smile2Unlock/2.0\r\n"
        L"Accept: application/vnd.github+json\r\n";
    if (!WinHttpSendRequest(static_cast<HINTERNET>(request.get()), kHeaders, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(static_cast<HINTERNET>(request.get()), nullptr)) {
        throw std::runtime_error("Failed to fetch latest release information.");
    }

    DWORD status_code = 0;
    DWORD status_code_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(static_cast<HINTERNET>(request.get()),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status_code,
                             &status_code_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        throw std::runtime_error("Failed to read update server response.");
    }
    if (status_code != 200) {
        throw std::runtime_error(std::format("Update server returned HTTP {}.", status_code));
    }

    std::string body;
    DWORD available_size = 0;
    do {
        if (!WinHttpQueryDataAvailable(static_cast<HINTERNET>(request.get()), &available_size)) {
            throw std::runtime_error("Failed while reading update response.");
        }
        if (available_size == 0) {
            break;
        }

        std::string chunk(static_cast<size_t>(available_size), '\0');
        DWORD bytes_read = 0;
        if (!WinHttpReadData(static_cast<HINTERNET>(request.get()), chunk.data(), available_size, &bytes_read)) {
            throw std::runtime_error("Failed while reading update response body.");
        }
        chunk.resize(bytes_read);
        body += chunk;
    } while (available_size > 0);

    return body;
}

VersionCheckResult FetchLatestVersionInfo(bool manual) {
    VersionCheckResult result;
    result.manual = manual;
    result.current_version = kCurrentVersion;
    result.release_url = kReleasesPageUrl;

    try {
        const std::string response = HttpGetUtf8(kLatestReleaseApiUrl);
        const auto tag_name = ExtractJsonStringField(response, "tag_name");
        if (!tag_name.has_value() || tag_name->empty()) {
            throw std::runtime_error("Release tag was missing from the update response.");
        }

        result.remote_version = *tag_name;
        if (const auto html_url = ExtractJsonStringField(response, "html_url")) {
            result.release_url = *html_url;
        }
        if (const auto published_at = ExtractJsonStringField(response, "published_at")) {
            result.published_at = FormatIsoTimestamp(*published_at);
        }

        result.update_available = CompareVersions(result.current_version, result.remote_version) < 0;
        result.success = true;
    } catch (const std::exception& e) {
        result.error_message = e.what();
    }

    return result;
}

std::string GetLocalTimeStampString() {
    std::time_t now = std::time(nullptr);
    std::tm local_time{};
    localtime_s(&local_time, &now);
    std::ostringstream stream;
    stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

ImVec4 WithAlpha(const ImVec4& color, const float alpha) {
    return ImVec4(color.x, color.y, color.z, alpha);
}

UiLanguage ParseUiLanguage(std::string_view language_code) {
    if (language_code.starts_with("en")) {
        return UiLanguage::EnUS;
    }
    return UiLanguage::ZhCN;
}

const char* NormalizeLanguageCode(std::string_view language_code) {
    return ParseUiLanguage(language_code) == UiLanguage::EnUS ? "en-US" : "zh-CN";
}

template <typename... Args>
std::string DynFormat(const std::string_view format, Args&&... args) {
    return std::vformat(format, std::make_format_args(args...));
}

bool TryCopyWideStringToUtf8(_In_z_ const wchar_t* source,
                             _Out_writes_z_(target_size) char* target,
                             const size_t target_size) {
    if (source == nullptr || target == nullptr || target_size == 0) {
        return false;
    }

    const int required_size = WideCharToMultiByte(
        CP_UTF8, 0, source, -1, nullptr, 0, nullptr, nullptr);
    if (required_size <= 0 || static_cast<size_t>(required_size) > target_size) {
        return false;
    }

    return WideCharToMultiByte(
               CP_UTF8, 0, source, -1, target, required_size, nullptr, nullptr) > 0;
}

bool TryGetUtf8WindowsUsername(_Out_writes_z_(buffer_size) char* buffer,
                               const size_t buffer_size) {
    if (buffer == nullptr || buffer_size == 0) {
        return false;
    }

    WCHAR username[256]{};
    DWORD username_length = ARRAYSIZE(username);
    if (GetUserNameW(username, &username_length) && username_length > 0) {
        return TryCopyWideStringToUtf8(username, buffer, buffer_size);
    }

    return false;
}

bool TryReadMicrosoftAccountEmailFromRegistry(_Out_writes_z_(buffer_size) char* buffer,
                                              const size_t buffer_size) {
    if (buffer == nullptr || buffer_size == 0) {
        return false;
    }

    const wchar_t* registry_paths[] = {
        L"Software\\Microsoft\\IdentityCRL\\UserExtendedProperties",
        L"Software\\Microsoft\\IdentityCRL\\StoredIdentities"
    };

    for (const wchar_t* registry_path : registry_paths) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, registry_path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            continue;
        }

        DWORD index = 0;
        wchar_t subkey_name[512]{};
        DWORD subkey_name_length = ARRAYSIZE(subkey_name);
        while (RegEnumKeyExW(key, index, subkey_name, &subkey_name_length,
                             nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            if (wcschr(subkey_name, L'@') != nullptr) {
                if (TryCopyWideStringToUtf8(subkey_name, buffer, buffer_size)) {
                    RegCloseKey(key);
                    return true;
                }
            }

            ++index;
            subkey_name[0] = L'\0';
            subkey_name_length = ARRAYSIZE(subkey_name);
        }

        RegCloseKey(key);
    }

    return false;
}

bool TryGetMicrosoftAccountEmailFromToken(_Out_writes_z_(buffer_size) char* buffer,
                                          const size_t buffer_size) {
    if (buffer == nullptr || buffer_size == 0) {
        return false;
    }

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    DWORD required_size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required_size);
    if (required_size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(token);
        return false;
    }

    PTOKEN_USER token_user = static_cast<PTOKEN_USER>(LocalAlloc(LPTR, required_size));
    if (token_user == nullptr) {
        CloseHandle(token);
        return false;
    }

    if (!GetTokenInformation(token, TokenUser, token_user, required_size, &required_size)) {
        LocalFree(token_user);
        CloseHandle(token);
        return false;
    }

    wchar_t account_name[512]{};
    wchar_t domain_name[512]{};
    DWORD account_name_size = ARRAYSIZE(account_name);
    DWORD domain_name_size = ARRAYSIZE(domain_name);
    SID_NAME_USE sid_type = SidTypeUnknown;

    const BOOL lookup_ok = LookupAccountSidW(
        nullptr,
        token_user->User.Sid,
        account_name,
        &account_name_size,
        domain_name,
        &domain_name_size,
        &sid_type);
    LocalFree(token_user);
    CloseHandle(token);

    if (!lookup_ok) {
        return false;
    }

    if (_wcsicmp(domain_name, L"MicrosoftAccount") != 0) {
        return false;
    }

    if (wcschr(account_name, L'@') != nullptr) {
        return TryCopyWideStringToUtf8(account_name, buffer, buffer_size);
    }

    return TryReadMicrosoftAccountEmailFromRegistry(buffer, buffer_size);
}

bool IsCurrentUserLocalBySid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    DWORD required_size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required_size);
    if (required_size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(token);
        return false;
    }

    PTOKEN_USER token_user = static_cast<PTOKEN_USER>(LocalAlloc(LPTR, required_size));
    if (token_user == nullptr) {
        CloseHandle(token);
        return false;
    }

    if (!GetTokenInformation(token, TokenUser, token_user, required_size, &required_size)) {
        LocalFree(token_user);
        CloseHandle(token);
        return false;
    }
    CloseHandle(token);

    WCHAR account_name[256]{};
    WCHAR domain_name[256]{};
    DWORD account_name_length = ARRAYSIZE(account_name);
    DWORD domain_name_length = ARRAYSIZE(domain_name);
    SID_NAME_USE sid_type = SidTypeUnknown;

    const BOOL lookup_ok = LookupAccountSidW(
        nullptr,
        token_user->User.Sid,
        account_name,
        &account_name_length,
        domain_name,
        &domain_name_length,
        &sid_type);
    LocalFree(token_user);
    if (!lookup_ok) {
        return false;
    }

    WCHAR computer_name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD computer_name_length = ARRAYSIZE(computer_name);
    if (!GetComputerNameW(computer_name, &computer_name_length)) {
        return false;
    }

    return _wcsicmp(domain_name, computer_name) == 0;
}

bool TryGetDefaultCpUsername(_Out_writes_z_(buffer_size) char* buffer,
                             const size_t buffer_size) {
    if (buffer == nullptr || buffer_size == 0) {
        return false;
    }

    buffer[0] = '\0';

    char username[256]{};
    const bool has_username = TryGetUtf8WindowsUsername(username, sizeof(username));
    const bool is_local_user = IsCurrentUserLocalBySid();

    if (!is_local_user && TryGetMicrosoftAccountEmailFromToken(buffer, buffer_size)) {
        LOG_INFO(LOG_MODULE_SERVICE, "Default CP username source=microsoft-account-email value=%s", buffer);
        return true;
    }

    if (has_username) {
        strncpy_s(buffer, buffer_size, username, _TRUNCATE);
        LOG_INFO(LOG_MODULE_SERVICE, "Default CP username source=local-username value=%s", buffer);
        return true;
    }

    LOG_WARNING(LOG_MODULE_SERVICE, "Default CP username unavailable");
    return false;
}

bool EnsureApplicationMFInitialized() {
    static struct MFInitGuard {
        HRESULT hr{MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)};
        ~MFInitGuard() {
            if (SUCCEEDED(hr)) {
                MFShutdown();
            }
        }
    } guard;
    return SUCCEEDED(guard.hr);
}

std::vector<CameraDeviceOption> EnumerateCameraDevices(std::string& error_message) {
    std::vector<CameraDeviceOption> devices;
    error_message.clear();
    const bool english = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ENGLISH;

    if (!EnsureApplicationMFInitialized()) {
        error_message = english ? "Failed to initialize Media Foundation" : "Media Foundation 初始化失败";
        return devices;
    }

    IMFAttributes* attributes = nullptr;
    HRESULT hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) {
        error_message = english ? "Failed to create camera enumeration attributes" : "创建摄像头枚举属性失败";
        return devices;
    }

    hr = attributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
        attributes->Release();
        error_message = english ? "Failed to set camera enumeration attributes" : "设置摄像头枚举属性失败";
        return devices;
    }

    IMFActivate** activate_array = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attributes, &activate_array, &count);
    attributes->Release();

    if (FAILED(hr)) {
        error_message = english ? "Failed to enumerate camera devices" : "枚举摄像头设备失败";
        return devices;
    }

    devices.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* friendly_name = nullptr;
        UINT32 name_length = 0;
        std::string label = english ? "Camera " + std::to_string(i) : "摄像头 " + std::to_string(i);

        if (SUCCEEDED(activate_array[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                &friendly_name,
                &name_length)) &&
            friendly_name != nullptr) {
            const int required = WideCharToMultiByte(CP_UTF8, 0, friendly_name, -1, nullptr, 0, nullptr, nullptr);
            if (required > 1) {
                std::string utf8_name(static_cast<size_t>(required - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, friendly_name, -1, utf8_name.data(), required, nullptr, nullptr);
                label = utf8_name + (english ? "  [Index " : "  [索引 ") + std::to_string(i) + "]";
            }
            CoTaskMemFree(friendly_name);
        } else {
            label += (english ? "  [Index " : "  [索引 ") + std::to_string(i) + "]";
        }

        devices.push_back({static_cast<int>(i), label});
        activate_array[i]->Release();
    }

    if (activate_array != nullptr) {
        CoTaskMemFree(activate_array);
    }

    if (devices.empty()) {
        error_message = english ? "No camera devices detected" : "未检测到摄像头设备";
    }

    return devices;
}

Application::Application() : window_(nullptr), initialized_(false) {
    // 后端由外部设置，不再在此处创建
    char username[256]{};
    if (TryGetDefaultCpUsername(username, sizeof(username))) {
        strncpy_s(ui_state_.new_username, sizeof(ui_state_.new_username), username, _TRUNCATE);
    }
}

void Application::RefreshCameraDeviceList() {
    std::string error_message;
    ui_state_.camera_devices = EnumerateCameraDevices(error_message);
    ui_state_.camera_devices_loaded = true;
    if (!error_message.empty()) {
        ui_state_.camera_devices_message = error_message;
    } else {
        ui_state_.camera_devices_message = DynFormat(Tr("camera_detected"), ui_state_.camera_devices.size());
    }
}

void Application::SyncCameraPreviewState() {
    const bool preview_running = backend_ && backend_->IsCameraPreviewRunning();
    if (ui_state_.camera_enabled == preview_running) {
        return;
    }

    ui_state_.camera_enabled = preview_running;
    if (!preview_running) {
        if (ui_state_.preview_message.empty() ||
            ui_state_.preview_message == Tr("camera_live_preview") ||
            ui_state_.preview_message == Tr("camera_opened")) {
            ui_state_.preview_message = Tr("camera_not_running");
        }
        ClearPreviewTexture();
    } else if (ui_state_.preview_message.empty() ||
               ui_state_.preview_message == Tr("camera_not_running") ||
               ui_state_.preview_message == Tr("camera_closed")) {
        ui_state_.preview_message = Tr("camera_live_preview");
    }
}

Application::~Application() { Shutdown(); }

bool Application::IsEnglishSystemUiLanguage() {
    const LANGID language = GetUserDefaultUILanguage();
    return PRIMARYLANGID(language) == LANG_ENGLISH;
}

std::unordered_map<std::string, std::string> Application::LoadLanguageFile(
    const std::string& language_code,
    std::string& error_message) const {
    std::unordered_map<std::string, std::string> translations;
    error_message.clear();

    const auto path = smile2unlock::paths::GetResourcesDirectory() / "lang" / (language_code + ".ini");
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        error_message = "Failed to open language file: " + path.string();
        return translations;
    }

    // Read entire file
    std::string content(std::istreambuf_iterator<char>{file}, {});

    // Handle UTF-8 BOM
    if (content.starts_with("\xEF\xBB\xBF")) {
        content.erase(0, 3);
    }

    // Split into lines and process
    std::string_view content_view = content;
    size_t start = 0;
    while (start < content_view.size()) {
        size_t end = content_view.find('\n', start);
        std::string_view line = end == std::string_view::npos
            ? content_view.substr(start)
            : content_view.substr(start, end - start);
        start = end == std::string_view::npos ? content_view.size() : end + 1;

        // Remove trailing carriage return
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        // Skip empty lines and comments
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        // Split key and value
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string_view::npos) {
            continue;
        }

        std::string_view key = line.substr(0, eq_pos);
        std::string_view value = line.substr(eq_pos + 1);

        // Trim whitespace from key and value
        auto trim = [](std::string_view sv) -> std::string_view {
            size_t start = 0;
            while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start]))) {
                ++start;
            }
            size_t end = sv.size();
            while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) {
                --end;
            }
            return sv.substr(start, end - start);
        };

        std::string_view trimmed_key = trim(key);
        std::string_view trimmed_value = trim(value);

        if (!trimmed_key.empty()) {
            translations[std::string(trimmed_key)] = std::string(trimmed_value);
        }
    }

    return translations;
}
void Application::LoadRecognizerConfigOnce() {
    if (!ui_state_.recognizer_config_loaded) {
        std::string error;
        if (backend_->GetRecognizerConfig(ui_state_.recognizer_config, error)) {
            ui_state_.recognizer_config.language = ResolveInitialLanguage(ui_state_.recognizer_config.language);
            ApplyLanguage(ui_state_.recognizer_config.language);
            ui_state_.recognizer_saved_config = ui_state_.recognizer_config;
            ui_state_.recognizer_config_loaded = true;
            ui_state_.recognizer_config_dirty = false;
            ui_state_.recognizer_config_message = Tr("config_loaded");
        } else {
            ui_state_.recognizer_config_message = DynFormat(Tr("config_load_failed"), error);
        }
    }
    if (!ui_state_.camera_devices_loaded) {
        RefreshCameraDeviceList();
    }
}

void Application::StartVersionCheck(const bool manual) {
    if (ui_state_.version_check_in_progress) {
        return;
    }

    ui_state_.version_check_in_progress = true;
    ui_state_.version_check_started = true;
    ui_state_.version_check.manual = manual;
    ui_state_.version_check.error_message.clear();
    ui_state_.version_check.current_version = kCurrentVersion;
    ui_state_.version_check_future = std::async(std::launch::async, [manual]() {
        return FetchLatestVersionInfo(manual);
    });
}

void Application::PollVersionCheck() {
    if (!ui_state_.version_check_in_progress || !ui_state_.version_check_future.valid()) {
        return;
    }

    if (ui_state_.version_check_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    ui_state_.version_check = ui_state_.version_check_future.get();
    ui_state_.version_check_in_progress = false;
    ui_state_.version_last_checked_at = GetLocalTimeStampString();

    if (ui_state_.version_check.success && ui_state_.version_check.update_available) {
        ui_state_.version_update_prompt_open = true;
        ui_state_.status_message = DynFormat(
            Tr("update_available_status"),
            ui_state_.version_check.remote_version,
            ui_state_.version_check.current_version);
        ui_state_.status_message_timer = 6.0f;
    } else if (ui_state_.version_check.manual) {
        ui_state_.status_message = ui_state_.version_check.success
            ? Tr("already_latest")
            : DynFormat(Tr("update_check_failed"), ui_state_.version_check.error_message);
        ui_state_.status_message_timer = 4.0f;
    }
}

void Application::OpenLatestReleasePage() const {
    const std::string target_url = ui_state_.version_check.release_url.empty()
        ? std::string(kReleasesPageUrl)
        : ui_state_.version_check.release_url;
    const std::wstring wide_url = Utf8ToWide(target_url);
    if (!wide_url.empty()) {
        ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void Application::RenderVersionPanel() {
    RenderSectionHeader(Tr("section_update"), Tr("update_title"), Tr("update_body"));

    RenderInfoRow(Tr("current_version"), ui_state_.version_check.current_version.empty()
        ? std::string(kCurrentVersion)
        : ui_state_.version_check.current_version, Nord8);

    if (ui_state_.version_check_in_progress) {
        RenderInfoRow(Tr("latest_version"), Tr("checking_update"), Nord13);
    } else if (!ui_state_.version_check.remote_version.empty()) {
        const ImVec4 remote_color = ui_state_.version_check.update_available ? Nord13 : Nord14;
        RenderInfoRow(Tr("latest_version"), ui_state_.version_check.remote_version, remote_color);
    } else {
        RenderInfoRow(Tr("latest_version"), Tr("update_unknown"), Nord3);
    }

    if (!ui_state_.version_last_checked_at.empty()) {
        RenderInfoRow(Tr("last_checked"), ui_state_.version_last_checked_at, Nord7);
    }
    if (!ui_state_.version_check.published_at.empty()) {
        RenderInfoRow(Tr("release_published_at"), ui_state_.version_check.published_at, Nord7);
    }

    std::string update_state = Tr("update_not_checked");
    ImVec4 update_state_color = Nord3;
    if (ui_state_.version_check_in_progress) {
        update_state = Tr("checking_update");
        update_state_color = Nord13;
    } else if (!ui_state_.version_check.success && ui_state_.version_check_started) {
        update_state = DynFormat(Tr("update_check_failed"), ui_state_.version_check.error_message);
        update_state_color = Nord11;
    } else if (ui_state_.version_check.success && ui_state_.version_check.update_available) {
        update_state = DynFormat(Tr("update_available_fmt"), ui_state_.version_check.remote_version);
        update_state_color = Nord13;
    } else if (ui_state_.version_check.success) {
        update_state = Tr("already_latest");
        update_state_color = Nord14;
    }
    RenderInfoRow(Tr("update_state"), update_state, update_state_color);

    ImGui::Spacing();
    const float check_width = CalcButtonWidth(Tr("check_update_now"), 190.0f);
    const float open_width = CalcButtonWidth(Tr("open_release_page"), 210.0f);
    if (ui_state_.version_check_in_progress) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(Tr("check_update_now"), ImVec2(check_width, 0))) {
        StartVersionCheck(true);
    }
    if (ui_state_.version_check_in_progress) {
        ImGui::EndDisabled();
    }
    if (CanFitInline({check_width, open_width})) {
        ImGui::SameLine(0.0f, 12.0f);
    }
    if (ImGui::Button(Tr("open_release_page"), ImVec2(open_width, 0))) {
        OpenLatestReleasePage();
    }

    const char* popup_name = "VersionUpdatePrompt";
    if (ui_state_.version_update_prompt_open) {
        ImGui::OpenPopup(popup_name);
        ui_state_.version_update_prompt_open = false;
    }
    if (ImGui::BeginPopupModal(popup_name, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        RenderSectionHeader(Tr("section_update"), Tr("update_dialog_title"));
        ImGui::TextWrapped("%s", DynFormat(
            Tr("update_dialog_body"),
            ui_state_.version_check.current_version,
            ui_state_.version_check.remote_version).c_str());
        ImGui::Spacing();
        if (ImGui::Button(Tr("open_release_page"), ImVec2(CalcButtonWidth(Tr("open_release_page"), 210.0f), 0))) {
            OpenLatestReleasePage();
        }
        ImGui::SameLine(0.0f, 12.0f);
        if (ImGui::Button(Tr("close_button"), ImVec2(CalcButtonWidth(Tr("close_button"), 120.0f), 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

std::string Application::ResolveInitialLanguage(std::string_view configured_language) const {
    if (!configured_language.empty()) {
        return std::string(NormalizeLanguageCode(configured_language));
    }
    return IsEnglishSystemUiLanguage() ? "en-US" : "zh-CN";
}

bool Application::ApplyLanguage(std::string_view requested_language) {
    const std::string desired_language = std::string(NormalizeLanguageCode(requested_language));
    if (fallback_translations_.empty()) {
        std::string fallback_error;
        fallback_translations_ = LoadLanguageFile("zh-CN", fallback_error);
        if (fallback_translations_.empty()) {
            LOG_ERROR(LOG_MODULE_SERVICE, "Failed to load fallback language file: %s", fallback_error.c_str());
            return false;
        }
    }

    std::string error_message;
    auto translations = LoadLanguageFile(desired_language, error_message);
    if (translations.empty()) {
        LOG_WARNING(LOG_MODULE_SERVICE, "Failed to load language '%s': %s", desired_language.c_str(), error_message.c_str());
        if (desired_language != "zh-CN") {
            translations = fallback_translations_;
            active_language_code_ = "zh-CN";
        } else {
            return false;
        }
    } else {
        active_language_code_ = desired_language;
    }

    active_translations_ = std::move(translations);
    ui_state_.recognizer_config.language = active_language_code_;
    return true;
}

float Application::CalcButtonWidth(std::string_view label, float min_width) const {
    return std::max(min_width, ImGui::CalcTextSize(label.data()).x + 40.0f);
}

bool Application::CanFitInline(std::initializer_list<float> widths, float spacing) const {
    float total = 0.0f;
    bool first = true;
    for (const float width : widths) {
        if (!first) {
            total += spacing;
        }
        total += width;
        first = false;
    }
    return total <= ImGui::GetContentRegionAvail().x;
}

bool Application::Initialize() {
    if (!backend_->Initialize()) return false;
    std::string config_message;
    if (backend_->GetRecognizerConfig(ui_state_.recognizer_config, config_message)) {
        ui_state_.recognizer_config.language = ResolveInitialLanguage(ui_state_.recognizer_config.language);
        ui_state_.recognizer_saved_config = ui_state_.recognizer_config;
        ui_state_.recognizer_config_loaded = true;
        ui_state_.recognizer_config_dirty = false;
    } else {
        ui_state_.recognizer_config.language = ResolveInitialLanguage({});
        ui_state_.recognizer_saved_config.language = ui_state_.recognizer_config.language;
    }

    if (!ApplyLanguage(ui_state_.recognizer_config.language)) {
        return false;
    }
    if (!glfwInit()) return false;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    window_ = glfwCreateWindow(1600, 900, Tr("window_title"), nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    imgui_ini_path_ = smile2unlock::paths::GetImguiIniPath().string();
    io.IniFilename = imgui_ini_path_.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 24.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    if (io.Fonts->Fonts.Size == 0) {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = 2.5f;
    }
    ApplyNordStyle();
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    ui_state_.version_check.current_version = kCurrentVersion;
    if (ui_state_.recognizer_config.auto_update_check) {
        StartVersionCheck(false);
    }
    initialized_ = true;
    return true;
}

void Application::Run() {
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        RenderUI();
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window_, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(Nord0.x, Nord0.y, Nord0.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
    }
}

void Application::Shutdown() {
    if (!initialized_) return;
    ClearPreviewTexture();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
    initialized_ = false;
}

void Application::ClearPreviewTexture() {
    if (ui_state_.preview_texture != 0) {
        glDeleteTextures(1, &ui_state_.preview_texture);
        ui_state_.preview_texture = 0;
    }
    ui_state_.preview_width = 0;
    ui_state_.preview_height = 0;
    ui_state_.has_preview = false;
}

void Application::UpdatePreviewTexture(const std::vector<unsigned char>& image_data, int width, int height) {
    if (image_data.empty() || width <= 0 || height <= 0) {
        ClearPreviewTexture();
        return;
    }

    if (ui_state_.preview_texture == 0) {
        glGenTextures(1, &ui_state_.preview_texture);
    }

    glBindTexture(GL_TEXTURE_2D, ui_state_.preview_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, image_data.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    ui_state_.preview_width = width;
    ui_state_.preview_height = height;
    ui_state_.has_preview = true;
}

void Application::RefreshFacePreview() {
    std::vector<unsigned char> image_data;
    int width = 0;
    int height = 0;
    std::string error_message;
    if (backend_->CapturePreviewFrame(image_data, width, height, error_message)) {
        UpdatePreviewTexture(image_data, width, height);
        ui_state_.preview_message = Tr("preview_updated");
    } else {
        ui_state_.preview_message = error_message.empty() ? Tr("cannot_get_preview") : error_message;
    }
}

void Application::PollPreviewStream() {
    SyncCameraPreviewState();
    if (!ui_state_.camera_enabled) {
        return;
    }

    std::vector<unsigned char> image_data;
    int width = 0;
    int height = 0;
    std::string error_message;
    if (backend_->GetLatestCameraPreview(image_data, width, height, error_message)) {
        UpdatePreviewTexture(image_data, width, height);
        ui_state_.preview_message = Tr("camera_live_preview");
    } else if (!error_message.empty()) {
        ui_state_.preview_message = error_message;
    }
}

void Application::RenderUI() {
    glfwSetWindowTitle(window_, Tr("window_title"));
    PollVersionCheck();
    if (ui_state_.dll_status_refresh_timer < 0 || ui_state_.dll_status_refresh_timer > 2.0f) {
        ui_state_.cached_dll_status = backend_->GetDllStatus();
        ui_state_.dll_status_refresh_timer = 0.0f;
    }
    ui_state_.dll_status_refresh_timer += ImGui::GetIO().DeltaTime;

    RenderShell();

}

void Application::RenderShell() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin(Tr("window_title"), nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus);

    RenderTitleBar();
    ImGui::Spacing();

    const float content_height = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("Sidebar", ImVec2(380.0f, content_height), true);
    RenderSidebar();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("MainContent", ImVec2(0, content_height), false);
    RenderOverview();
    ImGui::Spacing();

    if (ui_state_.active_panel == PanelInjector) {
        RenderInjectorPanel();
    } else if (ui_state_.active_panel == PanelUsers) {
        RenderUsersPanel();
    } else if (ui_state_.active_panel == PanelRecognizer) {
        RenderRecognizerPanel();
    } else {
        RenderConfigPanel();
    }

    RenderStatusMessage();
    ImGui::EndChild();

    ImGui::End();
    if (ui_state_.show_demo_window) ImGui::ShowDemoWindow(&ui_state_.show_demo_window);
}

void Application::RenderTitleBar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, WithAlpha(Nord1, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(Nord3, 0.65f));
    ImGui::BeginChild("TitleBar", ImVec2(0, 148.0f), true, kStaticChildFlags);

    const bool is_maximized = glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE;
    const char* current_panel =
        ui_state_.active_panel == PanelInjector ? Tr("panel_injector") :
        ui_state_.active_panel == PanelUsers ? Tr("panel_users") :
        ui_state_.active_panel == PanelRecognizer ? Tr("panel_recognizer")
                                                  : Tr("panel_config");

    ImGui::PushStyleColor(ImGuiCol_Text, Nord6);
    ImGui::TextUnformatted("Smile2Unlock");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.72f));
    ImGui::TextUnformatted(Tr("control_surface"));
    ImGui::PopStyleColor();

    const float button_width = 52.0f;
    const float button_spacing = 8.0f;
    const float total_buttons_width = button_width * 3.0f + button_spacing * 2.0f;
    const float text_region_width =
        ImGui::GetWindowWidth() - total_buttons_width - ImGui::GetStyle().WindowPadding.x * 3.0f;
    ImGui::SetCursorPos(ImVec2(
        ImGui::GetWindowWidth() - total_buttons_width - ImGui::GetStyle().WindowPadding.x,
        18.0f
    ));
    if (RenderWindowControlButton(WindowControlMinimize, WithAlpha(Nord3, 0.85f), WithAlpha(Nord9, 0.78f), button_width)) {
        glfwIconifyWindow(window_);
    }
    ImGui::SameLine(0.0f, button_spacing);
    if (RenderWindowControlButton(is_maximized ? WindowControlRestore : WindowControlMaximize, WithAlpha(Nord10, 0.80f), WithAlpha(Nord8, 0.92f), button_width)) {
        if (is_maximized) {
            glfwRestoreWindow(window_);
        } else {
            glfwMaximizeWindow(window_);
        }
    }
    ImGui::SameLine(0.0f, button_spacing);
    if (RenderWindowControlButton(WindowControlClose, WithAlpha(Nord11, 0.78f), WithAlpha(Nord11, 0.95f), button_width)) {
        glfwSetWindowShouldClose(window_, true);
    }

    ImGui::SetCursorPosY(58.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.78f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(320.0f, text_region_width));
    const std::string title_status = DynFormat(
        Tr("title_status_line"),
        current_panel,
        is_remote_backend_ ? Tr("backend_remote") : Tr("backend_standalone"),
        is_maximized ? Tr("window_maximized") : Tr("window_normal"));
    ImGui::TextWrapped("%s", title_status.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    ImGui::SetCursorPosY(98.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord9, 0.88f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(320.0f, text_region_width));
    ImGui::TextWrapped("%s",
        ui_state_.cached_dll_status.is_injected
            ? Tr("cp_ready_hint")
            : Tr("cp_not_ready_hint"));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    const bool title_bar_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool mouse_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool mouse_double_clicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool item_hovered = ImGui::IsAnyItemHovered();
    const bool item_active = ImGui::IsAnyItemActive();
    if (title_bar_hovered && !item_hovered && !item_active) {
        if (mouse_double_clicked) {
            if (is_maximized) {
                glfwRestoreWindow(window_);
            } else {
                glfwMaximizeWindow(window_);
            }
        } else if (mouse_clicked) {
            HWND hwnd = glfwGetWin32Window(window_);
            if (hwnd != nullptr) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

void Application::RenderSidebar() {
    RenderSectionHeader(Tr("section_navigation"), Tr("nav_title"), Tr("nav_body"));
    if (RenderNavItem("NavInjector", Tr("panel_injector"), Tr("nav_injector_hint"), ui_state_.active_panel == PanelInjector)) {
        ui_state_.active_panel = PanelInjector;
    }
    if (RenderNavItem("NavUsers", Tr("panel_users"), Tr("nav_users_hint"), ui_state_.active_panel == PanelUsers)) {
        ui_state_.active_panel = PanelUsers;
    }
    if (RenderNavItem("NavRecognizer", Tr("panel_recognizer"), Tr("nav_recognizer_hint"), ui_state_.active_panel == PanelRecognizer)) {
        ui_state_.active_panel = PanelRecognizer;
    }
    if (RenderNavItem("NavConfig", Tr("panel_config"), Tr("nav_config_hint"), ui_state_.active_panel == PanelConfig)) {
        ui_state_.active_panel = PanelConfig;
    }

    ImGui::Spacing();
    BeginPanelCard("SidebarState");
    RenderSectionHeader(Tr("section_status"), Tr("current_environment"));
    RenderInfoRow(Tr("backend_mode"), is_remote_backend_ ? Tr("backend_remote") : Tr("backend_standalone"));
    RenderInfoRow(Tr("dll_injection"), ui_state_.cached_dll_status.is_injected ? Tr("done") : Tr("pending"), ui_state_.cached_dll_status.is_injected ? Nord14 : Nord13);
    RenderInfoRow(Tr("camera_preview"), ui_state_.camera_enabled ? Tr("streaming") : Tr("disabled"), ui_state_.camera_enabled ? Nord8 : Nord3);
    EndPanelCard();
}

void Application::RenderOverview() {
    auto users = backend_->GetAllUsers();
    const int face_count = std::accumulate(users.begin(), users.end(), 0, [](const int total, const User& user) {
        return total + static_cast<int>(user.faces.size());
    });

    RenderSectionHeader(Tr("section_overview"), Tr("overview_title"), Tr("overview_body"));
    const float total_width = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float tile_width = (total_width - gap * 2.0f) / 3.0f;

    RenderStatTile("OverviewUsers", Tr("user_count"), std::to_string(users.size()), Tr("registered_accounts"), Nord8, tile_width);
    ImGui::SameLine(0.0f, gap);
    RenderStatTile("OverviewFaces", Tr("face_samples"), std::to_string(face_count), Tr("db_total_records"), Nord14, tile_width);
    ImGui::SameLine(0.0f, gap);
    RenderStatTile("OverviewDll", Tr("deployment_status"), ui_state_.cached_dll_status.is_registry_configured ? Tr("ready") : Tr("pending_en"), "CredentialProvider", ui_state_.cached_dll_status.is_registry_configured ? Nord9 : Nord13, tile_width);
}

void Application::RenderInjectorPanel() {
    const auto& status = ui_state_.cached_dll_status;

    BeginPanelCard("InjectorPanel");
    RenderSectionHeader(Tr("section_deployment"), Tr("cp_dll_title"), Tr("cp_dll_body"));
    RenderInfoRow(Tr("injection_status"), status.is_injected ? Tr("injected") : Tr("not_injected"), status.is_injected ? Nord14 : Nord11);
    RenderInfoRow(Tr("registry_status"), status.is_registry_configured ? Tr("configured") : Tr("not_configured"), status.is_registry_configured ? Nord14 : Nord13);
    RenderInfoRow(Tr("source_dll_path"), status.source_path);
    RenderInfoRow(Tr("target_dll_path"), status.target_path);
    if (!status.source_hash.empty()) {
        RenderInfoRow(Tr("source_dll_hash"), status.source_hash, Nord7);
        RenderInfoRow(Tr("target_dll_hash"), status.target_hash, Nord7);
        RenderInfoRow(Tr("source_dll_version"), status.source_version);
        RenderInfoRow(Tr("target_dll_version"), status.target_version);
    }

    ImGui::Spacing();
    const float refresh_width = CalcButtonWidth(Tr("refresh_deployment"), 220.0f);
    const float inject_width = CalcButtonWidth(Tr("inject_and_fix"), 280.0f);
    if (ImGui::Button(Tr("refresh_deployment"), ImVec2(refresh_width, 0))) {
        ui_state_.cached_dll_status = backend_->GetDllStatus();
        ui_state_.dll_status_refresh_timer = 0.0f;
    }
    if (CanFitInline({refresh_width, inject_width})) {
        ImGui::SameLine();
    }
    if (!status.is_injected || !status.is_registry_configured) {
        ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(Nord12, 0.82f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(Nord12, 0.96f));
        if (ImGui::Button(Tr("inject_and_fix"), ImVec2(inject_width, 0))) {
            std::string error;
            ui_state_.status_message = backend_->InjectDll(error) ? Tr("deploy_finished") : DynFormat(Tr("deploy_failed"), error);
            ui_state_.status_message_timer = 5.0f;
            ui_state_.dll_status_refresh_timer = -1.0f;
        }
        ImGui::PopStyleColor(2);
    }
    EndPanelCard();
}

void Application::RenderUsersPanel() {
    SyncCameraPreviewState();
    BeginPanelCard("UsersPanel");
    RenderSectionHeader(Tr("section_users"), Tr("users_title"), Tr("users_body"));
    auto users = backend_->GetAllUsers();
    RenderInfoRow(Tr("registered_users"),
                  DynFormat(Tr("users_count_fmt"), static_cast<int>(users.size())),
                  Nord8);

    if (ImGui::BeginTable("UsersTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn(Tr("login_username"), ImGuiTableColumnFlags_WidthStretch, 1.15f);
        ImGui::TableSetupColumn(Tr("faces_column"), ImGuiTableColumnFlags_WidthFixed, 136.0f);
        ImGui::TableSetupColumn(Tr("remark"), ImGuiTableColumnFlags_WidthStretch, 1.35f);
        ImGui::TableSetupColumn(Tr("actions"), ImGuiTableColumnFlags_WidthStretch, 1.05f);
        ImGui::TableSetupColumn(Tr("face_management"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();
        for (auto& user : users) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", user.id);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", user.username.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", DynFormat(Tr("faces_count_fmt"), static_cast<int>(user.faces.size())).c_str());
            ImGui::TableSetColumnIndex(3); ImGui::TextWrapped("%s", user.remark.c_str());
            ImGui::TableSetColumnIndex(4);
            const std::string edit_button = std::string(Tr("edit_button")) + "##user_" + std::to_string(user.id);
            if (ImGui::Button(edit_button.c_str(), ImVec2(CalcButtonWidth(Tr("edit_button"), 96.0f), 0.0f))) {
                ui_state_.editing_user_id = user.id;
                strncpy_s(ui_state_.edit_username, sizeof(ui_state_.edit_username), user.username.c_str(), _TRUNCATE);
                memset(ui_state_.edit_password, 0, sizeof(ui_state_.edit_password));
                strncpy_s(ui_state_.edit_remark, sizeof(ui_state_.edit_remark), user.remark.c_str(), _TRUNCATE);
                ui_state_.show_edit_user = true;
            }
            ImGui::SameLine(0.0f, 10.0f);
            const std::string delete_button = std::string(Tr("delete_button")) + "##user_" + std::to_string(user.id);
            if (ImGui::Button(delete_button.c_str(), ImVec2(CalcButtonWidth(Tr("delete_button"), 96.0f), 0.0f))) {
                std::string error; backend_->DeleteUser(user.id, error);
            }
            ImGui::TableSetColumnIndex(5);
            const std::string manage_faces_button = std::string(Tr("manage_faces_button")) + "##face_" + std::to_string(user.id);
            if (ImGui::Button(manage_faces_button.c_str(), ImVec2(CalcButtonWidth(Tr("manage_faces_button"), 150.0f), 0.0f))) {
                ui_state_.selected_user_id = user.id;
                ui_state_.show_face_capture = true;
                ui_state_.request_preview_refresh = false;
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    RenderSectionHeader(Tr("section_create"), Tr("create_user_title"));
    ImGui::PushItemWidth(-1.0f);
    ImGui::InputText(Tr("cp_login_username"), ui_state_.new_username, sizeof(ui_state_.new_username));
    ImGui::InputText(Tr("password"), ui_state_.new_password, sizeof(ui_state_.new_password), ImGuiInputTextFlags_Password);
    ImGui::InputText(Tr("remark"), ui_state_.new_remark, sizeof(ui_state_.new_remark));
    ImGui::PopItemWidth();
    if (ImGui::Button(Tr("add_user_button"), ImVec2(CalcButtonWidth(Tr("add_user_button"), 180.0f), 0))) {
        std::string error;
        if (backend_->AddUser(ui_state_.new_username, ui_state_.new_password, ui_state_.new_remark, error)) {
            ui_state_.status_message = Tr("user_added");
            memset(ui_state_.new_username, 0, sizeof(ui_state_.new_username));
            memset(ui_state_.new_password, 0, sizeof(ui_state_.new_password));
            memset(ui_state_.new_remark, 0, sizeof(ui_state_.new_remark));
        } else {
            ui_state_.status_message = DynFormat(Tr("add_failed"), error);
        }
        ui_state_.status_message_timer = 3.0f;
    }

    const std::string edit_popup_name = std::string(Tr("edit_user_title")) + "###EditUserModal";
    if (ui_state_.show_edit_user) {
        ImGui::OpenPopup(edit_popup_name.c_str());
        ui_state_.show_edit_user = false;
    }

    if (ImGui::BeginPopupModal(edit_popup_name.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        RenderSectionHeader(Tr("section_edit"), Tr("edit_user_header"));
        ImGui::PushItemWidth(std::max(420.0f, ImGui::GetContentRegionAvail().x * 0.72f));
        ImGui::InputText((std::string(Tr("cp_login_username")) + "##edit").c_str(), ui_state_.edit_username, sizeof(ui_state_.edit_username));
        ImGui::InputText((std::string(Tr("new_password")) + "##edit").c_str(), ui_state_.edit_password, sizeof(ui_state_.edit_password), ImGuiInputTextFlags_Password);
        ImGui::InputText((std::string(Tr("remark")) + "##edit").c_str(), ui_state_.edit_remark, sizeof(ui_state_.edit_remark));
        ImGui::PopItemWidth();
        ImGui::TextDisabled("%s", Tr("keep_password_hint"));
        ImGui::Spacing();
        if (ImGui::Button(Tr("save_button"), ImVec2(CalcButtonWidth(Tr("save_button"), 120.0f), 0))) {
            std::string error;
            if (backend_->UpdateUser(ui_state_.editing_user_id, ui_state_.edit_username, ui_state_.edit_password, ui_state_.edit_remark, error)) {
                ui_state_.status_message = Tr("user_updated");
                memset(ui_state_.edit_password, 0, sizeof(ui_state_.edit_password));
                ImGui::CloseCurrentPopup();
            } else {
                ui_state_.status_message = DynFormat(Tr("update_failed"), error);
            }
            ui_state_.status_message_timer = 3.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("cancel_button"), ImVec2(CalcButtonWidth(Tr("cancel_button"), 120.0f), 0))) {
            memset(ui_state_.edit_password, 0, sizeof(ui_state_.edit_password));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    const std::string face_popup_name = std::string(Tr("face_management")) + "###FaceManagementModal";
    if (ui_state_.show_face_capture) {
        SyncCameraPreviewState();
        ImGui::OpenPopup(face_popup_name.c_str());
        ui_state_.show_face_capture = false;
    }

    if (ImGui::BeginPopupModal(face_popup_name.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto all_users = backend_->GetAllUsers();
        auto user_it = std::find_if(all_users.begin(), all_users.end(), [this](const User& u) { return u.id == ui_state_.selected_user_id; });
        if (user_it != all_users.end()) {
            RenderSectionHeader(Tr("section_capture"), user_it->username.c_str(), Tr("capture_body"));
            auto faces = backend_->GetUserFaces(user_it->id);
            RenderInfoRow(Tr("enrolled_faces"), DynFormat(Tr("faces_count_fmt"), static_cast<int>(faces.size())), Nord8);
            ImGui::Spacing();
            if (ImGui::BeginTable("FacesTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("ID"); ImGui::TableSetupColumn(Tr("remark")); ImGui::TableSetupColumn(Tr("actions")); ImGui::TableHeadersRow();
                for (const auto& face : faces) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%d", face.id);
                    ImGui::TableSetColumnIndex(1); ImGui::TextWrapped("%s", face.remark.c_str());
                    ImGui::TableSetColumnIndex(2);
                    const std::string delete_face_button = std::string(Tr("delete_button")) + "##face_" + std::to_string(face.id);
                    if (ImGui::Button(delete_face_button.c_str())) {
                        std::string error; backend_->DeleteFace(user_it->id, face.id, error);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
            RenderSectionHeader(Tr("section_new_face"), Tr("new_face_title"));
            ImGui::PushItemWidth(std::max(420.0f, ImGui::GetContentRegionAvail().x * 0.72f));
            ImGui::InputText((std::string(Tr("remark")) + "##face").c_str(), ui_state_.face_remark, sizeof(ui_state_.face_remark));
            ImGui::PopItemWidth();
            PollPreviewStream();
            RenderInfoRow(Tr("preview_notes"), Tr("preview_notes_body"));
            if (!ui_state_.camera_enabled) {
                if (ImGui::Button(Tr("open_camera"), ImVec2(CalcButtonWidth(Tr("open_camera"), 160.0f), 0))) {
                    std::string error;
                    if (backend_->StartCameraPreview(error)) {
                        ui_state_.camera_enabled = true;
                        ui_state_.preview_message = error.empty() ? Tr("camera_opened") : error;
                    } else {
                        ui_state_.preview_message = error.empty() ? Tr("open_camera_failed") : error;
                    }
                }
            } else {
                if (ImGui::Button(Tr("close_camera"), ImVec2(CalcButtonWidth(Tr("close_camera"), 160.0f), 0))) {
                    backend_->StopCameraPreview();
                    ui_state_.camera_enabled = false;
                    ui_state_.preview_message = Tr("camera_closed");
                    ClearPreviewTexture();
                }
            }
            ImGui::SameLine();
            if (!ui_state_.camera_enabled) {
                if (ImGui::Button(Tr("single_capture"), ImVec2(CalcButtonWidth(Tr("single_capture"), 160.0f), 0))) {
                    RefreshFacePreview();
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::Button(Tr("single_capture"), ImVec2(CalcButtonWidth(Tr("single_capture"), 160.0f), 0));
                ImGui::EndDisabled();
            }
            if (!ui_state_.preview_message.empty()) {
                RenderInfoRow(Tr("preview_status"), ui_state_.preview_message, ui_state_.camera_enabled ? Nord8 : Nord13);
            }
            ImGui::Spacing();
            if (ui_state_.has_preview && ui_state_.preview_texture != 0) {
                const float max_width = 520.0f;
                const float scale = std::min(max_width / static_cast<float>(ui_state_.preview_width), 1.0f);
                const ImVec2 preview_size(
                    static_cast<float>(ui_state_.preview_width) * scale,
                    static_cast<float>(ui_state_.preview_height) * scale);
                ImGui::Image((ImTextureID)(intptr_t)ui_state_.preview_texture, preview_size);
            } else {
                RenderInfoRow(Tr("preview_frame"), Tr("no_preview"));
            }
            if (ImGui::Button(Tr("capture_and_enroll"), ImVec2(CalcButtonWidth(Tr("capture_and_enroll"), 180.0f), 0))) {
                std::string error_msg;
                bool success = backend_->CaptureAndAddFace(user_it->id, ui_state_.face_remark, error_msg);
                if (success) {
                    ui_state_.status_message = Tr("face_saved");
                    ui_state_.save_face_success = true;
                    memset(ui_state_.face_remark, 0, sizeof(ui_state_.face_remark));
                    if (!ui_state_.camera_enabled) {
                        ui_state_.request_preview_refresh = true;
                    }
                } else {
                    ui_state_.status_message = DynFormat(Tr("error_prefix"), error_msg);
                    ui_state_.save_face_success = false;
                }
                ui_state_.status_message_timer = 3.0f;
            }
        }
        ImGui::Spacing();
        if (ImGui::Button(Tr("close_button"), ImVec2(CalcButtonWidth(Tr("close_button"), 120.0f), 0))) {
            backend_->StopCameraPreview();
            ui_state_.camera_enabled = false;
            ui_state_.preview_message.clear();
            ClearPreviewTexture();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    EndPanelCard();
}

void Application::RenderRecognizerPanel() {
    SyncCameraPreviewState();
    BeginPanelCard("RecognizerPanel");
    RenderSectionHeader(Tr("section_service"), "FaceRecognizer", Tr("service_body"));

    LoadRecognizerConfigOnce();

    const bool recognition_running = backend_->IsRecognitionRunning();
    RenderInfoRow(Tr("recognition_process"), recognition_running ? Tr("running") : Tr("stopped"), recognition_running ? Nord14 : Nord3);
    RenderInfoRow(Tr("camera_preview"), ui_state_.camera_enabled ? Tr("active") : Tr("not_started"), ui_state_.camera_enabled ? Nord8 : Nord3);

    ImGui::Spacing();
    RenderSectionHeader(Tr("section_control"), Tr("control_title"), Tr("control_body"));
    const float start_width = CalcButtonWidth(Tr("start_fr"), 240.0f);
    const float stop_width = CalcButtonWidth(Tr("stop_fr"), 240.0f);
    if (ImGui::Button(Tr("start_fr"), ImVec2(start_width, 0))) {
        std::string error;
        ui_state_.status_message = backend_->StartRecognition(error) ? Tr("fr_started") : DynFormat(Tr("start_failed"), error);
        ui_state_.status_message_timer = 3.0f;
    }
    if (CanFitInline({start_width, stop_width})) {
        ImGui::SameLine(0.0f, 12.0f);
    }
    if (ImGui::Button(Tr("stop_fr"), ImVec2(stop_width, 0))) {
        backend_->StopRecognition();
        ui_state_.status_message = Tr("fr_stopped");
        ui_state_.status_message_timer = 3.0f;
    }
    EndPanelCard();
}

void Application::RenderConfigPanel() {
    SyncCameraPreviewState();
    BeginPanelCard("RecognizerConfigPanel");
    RenderSectionHeader(Tr("section_gui"), Tr("gui_title"), Tr("gui_body"));

    LoadRecognizerConfigOnce();

    auto mark_dirty = [this]() {
        ui_state_.recognizer_config_dirty =
            ui_state_.recognizer_config.camera != ui_state_.recognizer_saved_config.camera ||
            ui_state_.recognizer_config.liveness != ui_state_.recognizer_saved_config.liveness ||
            std::abs(ui_state_.recognizer_config.face_threshold - ui_state_.recognizer_saved_config.face_threshold) > 0.0001f ||
            std::abs(ui_state_.recognizer_config.liveness_threshold - ui_state_.recognizer_saved_config.liveness_threshold) > 0.0001f ||
            ui_state_.recognizer_config.debug != ui_state_.recognizer_saved_config.debug ||
            ui_state_.recognizer_config.language != ui_state_.recognizer_saved_config.language ||
            ui_state_.recognizer_config.auto_update_check != ui_state_.recognizer_saved_config.auto_update_check;
    };

    ui_state_.recognizer_config.language = NormalizeLanguageCode(ui_state_.recognizer_config.language);
    const std::array<std::pair<const char*, const char*>, 2> languages{{
        {"zh-CN", "language_option_zh_cn"},
        {"en-US", "language_option_en_us"}
    }};
    std::string selected_language_label = Tr(ui_state_.recognizer_config.language == "en-US"
                                                 ? "language_option_en_us"
                                                 : "language_option_zh_cn");
    if (ImGui::BeginCombo(Tr("ui_language"), selected_language_label.c_str())) {
        for (const auto& [code, label_key] : languages) {
            const bool selected = ui_state_.recognizer_config.language == code;
            const char* label = Tr(label_key);
            if (ImGui::Selectable(label, selected)) {
                ui_state_.recognizer_config.language = code;
                ApplyLanguage(code);
                ui_state_.recognizer_config_message = Tr("language_saved_note");
                mark_dirty();
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.72f));
    ImGui::TextWrapped("%s", Tr("gui_language_help"));
    ImGui::PopStyleColor();
    if (ImGui::Checkbox(Tr("auto_update_check"), &ui_state_.recognizer_config.auto_update_check)) {
        mark_dirty();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.72f));
    ImGui::TextWrapped("%s", Tr("auto_update_check_help"));
    ImGui::PopStyleColor();

    ImGui::Spacing();
    RenderVersionPanel();

    ImGui::Spacing();
    RenderSectionHeader(Tr("section_config"), Tr("config_title"), Tr("config_body"));

    RenderInfoRow(Tr("config_state"), ui_state_.recognizer_config_dirty ? Tr("unsaved_changes") : Tr("synced_to_disk"),
                  ui_state_.recognizer_config_dirty ? Nord13 : Nord14);

    std::string selected_camera_label = DynFormat(Tr("camera_not_listed_fmt"), ui_state_.recognizer_config.camera);
    for (const auto& device : ui_state_.camera_devices) {
        if (device.index == ui_state_.recognizer_config.camera) {
            selected_camera_label = device.label;
            break;
        }
    }

    if (ImGui::BeginCombo(Tr("camera_device"), selected_camera_label.c_str())) {
        for (const auto& device : ui_state_.camera_devices) {
            const bool selected = (device.index == ui_state_.recognizer_config.camera);
            if (ImGui::Selectable(device.label.c_str(), selected)) {
                ui_state_.recognizer_config.camera = device.index;
                mark_dirty();
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, 12.0f);
    if (ImGui::Button(Tr("refresh_devices"), ImVec2(CalcButtonWidth(Tr("refresh_devices"), 150.0f), 0))) {
        RefreshCameraDeviceList();
    }
    RenderInfoRow(Tr("current_camera_index"), std::to_string(ui_state_.recognizer_config.camera), Nord8);
    if (!ui_state_.camera_devices_message.empty()) {
        RenderInfoRow(Tr("device_enumeration"), ui_state_.camera_devices_message, ui_state_.camera_devices.empty() ? Nord13 : Nord14);
    }

    if (ImGui::Checkbox(Tr("enable_liveness"), &ui_state_.recognizer_config.liveness)) {
        mark_dirty();
    }
    if (ImGui::SliderFloat(Tr("face_threshold"), &ui_state_.recognizer_config.face_threshold, 0.30f, 0.95f, "%.2f")) {
        mark_dirty();
    }
    if (ImGui::SliderFloat(Tr("liveness_threshold"), &ui_state_.recognizer_config.liveness_threshold, 0.30f, 0.99f, "%.2f")) {
        mark_dirty();
    }
    if (ImGui::Checkbox(Tr("debug_log"), &ui_state_.recognizer_config.debug)) {
        mark_dirty();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.72f));
    ImGui::TextWrapped("%s", Tr("preset_hint"));
    ImGui::PopStyleColor();

    auto apply_preset = [this, &mark_dirty](int camera, bool liveness, float face, float live, bool debug, const char* label) {
        ui_state_.recognizer_config.camera = camera;
        ui_state_.recognizer_config.liveness = liveness;
        ui_state_.recognizer_config.face_threshold = face;
        ui_state_.recognizer_config.liveness_threshold = live;
        ui_state_.recognizer_config.debug = debug;
        ui_state_.recognizer_config_message = DynFormat(Tr("preset_applied"), label);
        mark_dirty();
    };

    const float preset_balanced_width = CalcButtonWidth(Tr("preset_balanced"), 160.0f);
    const float preset_strict_width = CalcButtonWidth(Tr("preset_strict"), 160.0f);
    const float preset_fast_width = CalcButtonWidth(Tr("preset_fast"), 160.0f);
    const bool presets_inline = CanFitInline({preset_balanced_width, preset_strict_width, preset_fast_width});
    if (ImGui::Button(Tr("preset_balanced"), ImVec2(preset_balanced_width, 0))) {
        apply_preset(std::max(0, ui_state_.recognizer_config.camera), true, 0.62f, 0.80f, false, Tr("preset_balanced"));
    }
    if (presets_inline) {
        ImGui::SameLine(0.0f, 12.0f);
    }
    if (ImGui::Button(Tr("preset_strict"), ImVec2(preset_strict_width, 0))) {
        apply_preset(std::max(0, ui_state_.recognizer_config.camera), true, 0.72f, 0.88f, false, Tr("preset_strict"));
    }
    if (presets_inline) {
        ImGui::SameLine(0.0f, 12.0f);
    }
    if (ImGui::Button(Tr("preset_fast"), ImVec2(preset_fast_width, 0))) {
        apply_preset(std::max(0, ui_state_.recognizer_config.camera), false, 0.55f, 0.65f, false, Tr("preset_fast"));
    }

    ImGui::Spacing();
    const float reload_width = CalcButtonWidth(Tr("reload_disk_config"), 220.0f);
    const float defaults_width = CalcButtonWidth(Tr("restore_defaults"), 180.0f);
    const float save_width = CalcButtonWidth(Tr("save_config"), 160.0f);
    const bool config_buttons_inline = CanFitInline({reload_width, defaults_width, save_width});
    if (ImGui::Button(Tr("reload_disk_config"), ImVec2(reload_width, 0))) {
        std::string error;
        if (backend_->GetRecognizerConfig(ui_state_.recognizer_config, error)) {
            ui_state_.recognizer_config.language = ResolveInitialLanguage(ui_state_.recognizer_config.language);
            ApplyLanguage(ui_state_.recognizer_config.language);
            ui_state_.recognizer_saved_config = ui_state_.recognizer_config;
            ui_state_.recognizer_config_loaded = true;
            ui_state_.recognizer_config_dirty = false;
            ui_state_.recognizer_config_message = Tr("reloaded_disk_config");
        } else {
            ui_state_.recognizer_config_message = DynFormat(Tr("reload_failed"), error);
        }
    }
    if (config_buttons_inline) {
        ImGui::SameLine(0.0f, 12.0f);
    }
    if (ImGui::Button(Tr("restore_defaults"), ImVec2(defaults_width, 0))) {
        ui_state_.recognizer_config.camera = 0;
        ui_state_.recognizer_config.liveness = true;
        ui_state_.recognizer_config.face_threshold = 0.62f;
        ui_state_.recognizer_config.liveness_threshold = 0.8f;
        ui_state_.recognizer_config.debug = false;
        ui_state_.recognizer_config.language = ResolveInitialLanguage({});
        ui_state_.recognizer_config.auto_update_check = true;
        ApplyLanguage(ui_state_.recognizer_config.language);
        ui_state_.recognizer_config_message = Tr("restored_defaults");
        mark_dirty();
    }
    if (config_buttons_inline) {
        ImGui::SameLine(0.0f, 12.0f);
    }
    if (ImGui::Button(Tr("save_config"), ImVec2(save_width, 0))) {
        std::string error;
        if (backend_->SaveRecognizerConfig(ui_state_.recognizer_config, error)) {
            ui_state_.recognizer_saved_config = ui_state_.recognizer_config;
            ui_state_.recognizer_config_dirty = false;
            ui_state_.recognizer_config_message = Tr("config_saved");
        } else {
            ui_state_.recognizer_config_message = DynFormat(Tr("save_failed"), error);
        }
        ui_state_.status_message = ui_state_.recognizer_config_message;
        ui_state_.status_message_timer = 3.0f;
    }

    if (!ui_state_.recognizer_config_message.empty()) {
        ImGui::Spacing();
        RenderInfoRow(Tr("config_feedback"), ui_state_.recognizer_config_message, ui_state_.recognizer_config_dirty ? Nord13 : Nord8);
    }

    EndPanelCard();
}

void Application::ApplyNordStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(2.0f);
    style.WindowPadding = ImVec2(20.0f, 20.0f);
    style.FramePadding = ImVec2(16.0f, 10.0f);
    style.ItemSpacing = ImVec2(14.0f, 14.0f);
    style.ItemInnerSpacing = ImVec2(10.0f, 10.0f);
    style.CellPadding = ImVec2(12.0f, 10.0f);
    style.ScrollbarSize = 22.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 10.0f;
    style.PopupRounding = 12.0f;
    style.TabRounding = 10.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = Nord0;
    colors[ImGuiCol_ChildBg] = WithAlpha(Nord1, 0.92f);
    colors[ImGuiCol_PopupBg] = WithAlpha(Nord1, 0.98f);
    colors[ImGuiCol_Border] = WithAlpha(Nord3, 0.55f);
    colors[ImGuiCol_FrameBg] = WithAlpha(Nord1, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = WithAlpha(Nord2, 1.0f);
    colors[ImGuiCol_FrameBgActive] = WithAlpha(Nord2, 1.0f);
    colors[ImGuiCol_TitleBg] = Nord1;
    colors[ImGuiCol_TitleBgActive] = Nord1;
    colors[ImGuiCol_Text] = Nord4;
    colors[ImGuiCol_TextDisabled] = Nord3;
    colors[ImGuiCol_Button] = WithAlpha(Nord10, 0.82f);
    colors[ImGuiCol_ButtonHovered] = WithAlpha(Nord9, 0.95f);
    colors[ImGuiCol_ButtonActive] = Nord8;
    colors[ImGuiCol_Header] = WithAlpha(Nord2, 1.0f);
    colors[ImGuiCol_HeaderHovered] = WithAlpha(Nord9, 0.35f);
    colors[ImGuiCol_HeaderActive] = WithAlpha(Nord9, 0.45f);
    colors[ImGuiCol_Separator] = WithAlpha(Nord3, 0.50f);
    colors[ImGuiCol_Tab] = WithAlpha(Nord1, 1.0f);
    colors[ImGuiCol_TabHovered] = WithAlpha(Nord2, 1.0f);
    colors[ImGuiCol_TabActive] = WithAlpha(Nord10, 0.90f);
    colors[ImGuiCol_TableHeaderBg] = WithAlpha(Nord1, 1.0f);
    colors[ImGuiCol_TableBorderStrong] = WithAlpha(Nord3, 0.65f);
    colors[ImGuiCol_TableBorderLight] = WithAlpha(Nord3, 0.35f);
    colors[ImGuiCol_TableRowBgAlt] = WithAlpha(Nord1, 0.50f);
    colors[ImGuiCol_ScrollbarBg] = WithAlpha(Nord1, 0.80f);
    colors[ImGuiCol_ScrollbarGrab] = WithAlpha(Nord3, 0.85f);
    colors[ImGuiCol_ScrollbarGrabHovered] = WithAlpha(Nord9, 0.90f);
    colors[ImGuiCol_ScrollbarGrabActive] = Nord8;
    colors[ImGuiCol_CheckMark] = Nord8;
    colors[ImGuiCol_SliderGrab] = Nord8;
    colors[ImGuiCol_SliderGrabActive] = Nord9;
    colors[ImGuiCol_TextSelectedBg] = WithAlpha(Nord8, 0.30f);
}

void Application::RenderSectionHeader(const char* eyebrow, const char* title, const char* body) {
    if (eyebrow && eyebrow[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, Nord9);
        ImGui::TextUnformatted(eyebrow);
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, Nord6);
    ImGui::TextWrapped("%s", title);
    ImGui::PopStyleColor();
    if (body && body[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.78f));
        ImGui::TextWrapped("%s", body);
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

void Application::RenderInfoRow(const char* label, const std::string& value, const ImVec4& value_color) {
    const float wrap_width = std::max(ImGui::GetContentRegionAvail().x - 28.0f, 180.0f);
    const float text_height = ImGui::CalcTextSize(value.c_str(), nullptr, false, wrap_width).y;
    const float row_height = std::max(84.0f, 48.0f + text_height + ImGui::GetStyle().WindowPadding.y * 0.7f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, WithAlpha(Nord0, 0.28f));
    ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(Nord3, 0.25f));
    ImGui::BeginChild(label, ImVec2(0, row_height), true, kStaticChildFlags);
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.62f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    const ImVec4 color = (value_color.w == 0.0f && value_color.x == 0.0f && value_color.y == 0.0f && value_color.z == 0.0f) ? Nord6 : value_color;
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", value.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

void Application::RenderStatTile(const char* id, const char* label, const std::string& value, const std::string& hint, const ImVec4& accent, float width) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, WithAlpha(Nord1, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(accent, 0.42f));
    ImGui::BeginChild(id, ImVec2(width, 136.0f), true, kStaticChildFlags);
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.65f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::TextWrapped("%s", value.c_str());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.75f));
    ImGui::TextWrapped("%s", hint.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

bool Application::RenderNavItem(const char* id, const char* label, const char* hint, bool selected) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? WithAlpha(Nord10, 0.36f) : WithAlpha(Nord1, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, selected ? WithAlpha(Nord8, 0.65f) : WithAlpha(Nord3, 0.30f));
    ImGui::BeginChild(id, ImVec2(0, 98.0f), true, kStaticChildFlags);
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? Nord6 : Nord4);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.72f));
    ImGui::TextWrapped("%s", hint);
    ImGui::PopStyleColor();
    const bool hovered = ImGui::IsWindowHovered();
    const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    return clicked;
}

bool Application::RenderWindowControlButton(int icon, const ImVec4& button_color, const ImVec4& hover_color, float width) {
    ImGui::PushID(icon);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(Nord6, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_Button, button_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, hover_color);
    const bool pressed = ImGui::Button("##WindowControl", ImVec2(width, width));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float icon_width = (max.x - min.x) * 0.26f;
    const float icon_height = (max.y - min.y) * 0.26f;
    const float thickness = 2.2f;
    const ImU32 color = ImGui::GetColorU32(Nord6);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    if (icon == WindowControlMinimize) {
        draw_list->AddLine(
            ImVec2(center.x - icon_width, center.y + icon_height * 0.55f),
            ImVec2(center.x + icon_width, center.y + icon_height * 0.55f),
            color,
            thickness
        );
    } else if (icon == WindowControlMaximize) {
        draw_list->AddRect(
            ImVec2(center.x - icon_width, center.y - icon_height),
            ImVec2(center.x + icon_width, center.y + icon_height),
            color,
            0.0f,
            0,
            thickness
        );
    } else if (icon == WindowControlRestore) {
        draw_list->AddRect(
            ImVec2(center.x - icon_width * 0.45f, center.y - icon_height * 0.95f),
            ImVec2(center.x + icon_width * 1.05f, center.y + icon_height * 0.55f),
            color,
            0.0f,
            0,
            thickness
        );
        draw_list->AddRect(
            ImVec2(center.x - icon_width * 1.05f, center.y - icon_height * 0.35f),
            ImVec2(center.x + icon_width * 0.45f, center.y + icon_height * 1.15f),
            color,
            0.0f,
            0,
            thickness
        );
    } else if (icon == WindowControlClose) {
        draw_list->AddLine(
            ImVec2(center.x - icon_width, center.y - icon_height),
            ImVec2(center.x + icon_width, center.y + icon_height),
            color,
            thickness
        );
        draw_list->AddLine(
            ImVec2(center.x + icon_width, center.y - icon_height),
            ImVec2(center.x - icon_width, center.y + icon_height),
            color,
            thickness
        );
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return pressed;
}

void Application::RenderStatusMessage() {
    if (ui_state_.status_message_timer <= 0.0f || ui_state_.status_message.empty()) {
        return;
    }
    ui_state_.status_message_timer -= ImGui::GetIO().DeltaTime;
    ImGui::Spacing();
    BeginPanelCard("StatusMessage");
    RenderSectionHeader(Tr("section_feedback"), Tr("recent_result"));
    RenderInfoRow(Tr("message"), ui_state_.status_message, Nord13);
    EndPanelCard();
}

void Application::BeginPanelCard(const char* id, float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, WithAlpha(Nord1, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(Nord3, 0.38f));
    ImGui::BeginChild(id, ImVec2(0, height), true);
}

void Application::EndPanelCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

const char* Application::Tr(std::string_view key) const {
    if (const auto active_it = active_translations_.find(std::string(key)); active_it != active_translations_.end()) {
        return active_it->second.c_str();
    }
    if (const auto fallback_it = fallback_translations_.find(std::string(key)); fallback_it != fallback_translations_.end()) {
        return fallback_it->second.c_str();
    }
        // Fallback to empty string instead of unsafe key.data()
    static const std::string empty;
    return empty.c_str();
}

} // namespace smile2unlock
