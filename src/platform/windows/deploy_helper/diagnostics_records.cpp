#include "diagnostics.h"
#include "platform/windows/deploy/deployment.h"
#include "common/diagnostics/export.h"
#include "common/log/windows_directory.h"
#include "common/utf8_path.h"
#include <winevt.h>
#include <wintrust.h>
#include <softpub.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <array>
#include <algorithm>
#include <cwctype>
#include <format>
namespace su::diagnostics {
namespace {
using Key = std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)>;
std::string utf8(std::wstring_view text) { return su::path_utf8(std::filesystem::path{text}); }
std::string registry_data(DWORD type, const std::vector<unsigned char>& bytes, DWORD size) {
    if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ) {
        const auto* chars = reinterpret_cast<const wchar_t*>(bytes.data());
        auto text = std::wstring(chars, size / sizeof(wchar_t));
        while (!text.empty() && !text.back())
            text.pop_back();
        for (auto& c : text)
            if (!c)
                c = L'|';
        return utf8(text);
    }
    auto text = std::string{"type="} + std::to_string(type) + " hex=";
    for (DWORD i = 0; i < size; ++i)
        text += std::format("{:02x}", bytes[i]);
    return text;
}
void read_registry(const std::wstring& path, bool recursive, Section& section, unsigned depth = 0) {
    if (depth > 16 || section.records.size() > 10000)
        return;
    HKEY raw = nullptr;
    const auto opened =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &raw);
    if (opened != ERROR_SUCCESS) {
        section.records.push_back(
            {"HKLM\\" + utf8(path), "unavailable Win32=" + std::to_string(opened)});
        return;
    }
    const auto key = Key{raw, &RegCloseKey};
    section.records.push_back({"HKLM\\" + utf8(path), "present"});
    for (DWORD i = 0; i < 4096; ++i) {
        wchar_t name[16384]{};
        DWORD count = 16384, type = 0, size = 65536;
        auto bytes = std::vector<unsigned char>(size + sizeof(wchar_t), 0);
        const auto result =
            RegEnumValueW(raw, i, name, &count, nullptr, &type, bytes.data(), &size);
        if (result == ERROR_NO_MORE_ITEMS)
            break;
        section.records.push_back({utf8(path) + "\\" + utf8(std::wstring_view{name, count}),
                                   result == ERROR_SUCCESS
                                       ? registry_data(type, bytes, size)
                                       : "read failed Win32=" + std::to_string(result)});
    }
    if (!recursive)
        return;
    for (DWORD i = 0; i < 4096; ++i) {
        wchar_t name[256]{};
        DWORD size = 256;
        if (RegEnumKeyExW(raw, i, name, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        read_registry(path + L"\\" + name, true, section, depth + 1);
    }
}
std::wstring inproc(const std::wstring& id) {
    wchar_t buffer[32768]{};
    DWORD size = sizeof(buffer);
    const auto path = L"SOFTWARE\\Classes\\CLSID\\" + id + L"\\InprocServer32";
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), nullptr,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer,
                     &size) != ERROR_SUCCESS)
        return {};
    return buffer;
}
void third_party_providers(Section& section) {
    constexpr auto providers =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers";
    HKEY raw = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, providers, 0, KEY_READ | KEY_WOW64_64KEY, &raw) !=
        ERROR_SUCCESS)
        return;
    const auto key = Key{raw, &RegCloseKey};
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    auto prefix = std::wstring{system} + L"\\";
    std::ranges::transform(prefix, prefix.begin(), towlower);
    for (DWORD i = 0; i < 4096; ++i) {
        wchar_t id[256]{};
        DWORD size = 256;
        if (RegEnumKeyExW(raw, i, id, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        const auto path = inproc(id);
        auto lower = path;
        std::ranges::transform(lower, lower.begin(), towlower);
        if (lower.empty() || !lower.starts_with(prefix))
            section.records.push_back({utf8(id), "InprocServer32=" + utf8(path)});
    }
}
Section registry_records() {
    auto section = Section{"Registry", {}};
    constexpr auto base = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\";
    constexpr auto id = L"{5fd3d285-0dd9-4362-8855-e0abaacd4af6}";
    read_registry(std::wstring(base) + L"Credential Providers\\" + id, false, section);
    read_registry(std::wstring(base) + L"CredentialProviders\\" + id, false, section);
    read_registry(std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + id, true, section);
    read_registry(std::wstring(base) + L"Credential Provider Filters", true, section);
    read_registry(L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", false, section);
    read_registry(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\PasswordLess\\Device", false,
                  section);
    read_registry(L"SOFTWARE\\Smile2Unlock", true, section);
    third_party_providers(section);
    return section;
}
std::expected<std::string, Error> sha256(HANDLE file) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return std::unexpected(Error{ErrorCode::io, "SHA-256 provider unavailable"});
    auto algorithm_guard = std::unique_ptr<void, void (*)(void*)>{
        algorithm, [](void* p) { BCryptCloseAlgorithmProvider(p, 0); }};
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0)
        return std::unexpected(Error{ErrorCode::io, "SHA-256 creation failed"});
    auto hash_guard =
        std::unique_ptr<void, void (*)(void*)>{hash, [](void* p) { BCryptDestroyHash(p); }};
    auto buffer = std::array<unsigned char, 65536>{};
    DWORD count = 0;
    for (;;) {
        if (!ReadFile(file, buffer.data(), buffer.size(), &count, nullptr))
            return std::unexpected(Error{ErrorCode::io, "hash read failed"});
        if (!count)
            break;
        if (BCryptHashData(hash, buffer.data(), count, 0) != 0)
            return std::unexpected(Error{ErrorCode::io, "hash update failed"});
    }
    unsigned char digest[32]{};
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0)
        return std::unexpected(Error{ErrorCode::io, "hash finish failed"});
    auto text = std::string{};
    for (const auto byte : digest)
        text += std::format("{:02x}", byte);
    return text;
}
std::string signature(const std::filesystem::path& path, HANDLE handle) {
    auto file = WINTRUST_FILE_INFO{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    file.hFile = handle;
    auto data = WINTRUST_DATA{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    auto action = GUID(WINTRUST_ACTION_GENERIC_VERIFY_V2);
    const auto result = WinVerifyTrust(nullptr, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return result == ERROR_SUCCESS
               ? "valid"
               : std::format("untrusted/unsigned (0x{:08x})", static_cast<unsigned long>(result));
}
Section installed_files() {
    auto section = Section{"Installed files", {}};
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &raw)))
        return {"Installed files", {{"error", "ProgramFiles unavailable"}}};
    const auto memory = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>{raw, &CoTaskMemFree};
    const auto bin = std::filesystem::path{raw} / L"Smile2Unlock/bin";
    const auto chain = su::log::check_directory_chain(bin, false);
    if (!chain)
        return {"Installed files", {{"error", "bin directory unavailable or rejected"}}};
    for (const auto& file : std::filesystem::directory_iterator(bin)) {
        auto extension = file.path().extension().wstring();
        std::ranges::transform(extension, extension.begin(), towlower);
        if (extension != L".exe" && extension != L".dll")
            continue;
        const auto handle =
            su::log::Handle{CreateFileW(file.path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!su::log::check_handle(handle.get(), false, false))
            continue;
        LARGE_INTEGER size{};
        GetFileSizeEx(handle.get(), &size);
        const auto digest = sha256(handle.get());
        section.records.push_back({su::path_utf8(file.path().filename()),
                                   "size=" + std::to_string(size.QuadPart) +
                                       " sha256=" + (digest ? *digest : digest.error().detail) +
                                       " Authenticode=" + signature(file.path(), handle.get())});
    }
    return section;
}
struct EventCloser {
    void operator()(void* event) const {
        if (event)
            EvtClose(event);
    }
};
using Event = std::unique_ptr<void, EventCloser>;
Section event_records(const wchar_t* channel, const wchar_t* query, bool logon) {
    auto section = Section{utf8(channel), {}};
    const auto events =
        Event{EvtQuery(nullptr, channel, query, EvtQueryChannelPath | EvtQueryReverseDirection)};
    if (!events)
        return {utf8(channel), {{"error", "EvtQuery Win32=" + std::to_string(GetLastError())}}};
    for (unsigned scanned = 0; scanned < 10000 && section.records.size() < 20; ++scanned) {
        EVT_HANDLE raw = nullptr;
        DWORD returned = 0;
        if (!EvtNext(events.get(), 1, &raw, 1000, 0, &returned))
            break;
        const auto event = Event{raw};
        DWORD used = 0, count = 0;
        EvtRender(nullptr, raw, EvtRenderEventXml, 0, nullptr, &used, &count);
        if (used > 1024 * 1024 || !used)
            continue;
        auto text = std::vector<wchar_t>((used + sizeof(wchar_t) - 1) / sizeof(wchar_t));
        if (!EvtRender(nullptr, raw, EvtRenderEventXml, used, text.data(), &used, &count))
            continue;
        auto xml = std::wstring{text.data()}, lower = xml;
        std::ranges::transform(lower, lower.begin(), towlower);
        if (lower.find(L"smile2unlock") != lower.npos ||
            (logon && lower.find(L"logonui") != lower.npos))
            section.records.push_back({"event", utf8(xml)});
    }
    return section;
}
Section legacy_log() {
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const auto path = std::filesystem::path{windows} / L"Temp/su_cp.log";
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return {"Legacy CP log",
                {{"existence", "unavailable Win32=" + std::to_string(GetLastError())}}};
    const auto size =
        (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return {"Legacy CP log", {{"exists", "yes"}, {"size", std::to_string(size)}}};
}
} // namespace
std::vector<Section> windows_system_records() {
    auto sections = std::vector<Section>{registry_records()};
    const auto deployment = su::windeploy::inspect_deployment();
    sections.push_back({"Deployment",
                        {{"snapshot", deployment ? su::windeploy::snapshot_json(*deployment)
                                                 : deployment.error()}}});
    sections.push_back(installed_files());
    sections.push_back(event_records(L"Application",
                                     L"*[System[(EventID=1000 or EventID=1001) and "
                                     L"TimeCreated[timediff(@SystemTime)<=604800000]]]",
                                     true));
    sections.push_back(event_records(L"Microsoft-Windows-CodeIntegrity/Operational",
                                     L"*[System[(EventID=3033 or EventID=3077 or EventID=3089) and "
                                     L"TimeCreated[timediff(@SystemTime)<=604800000]]]",
                                     false));
    sections.push_back(legacy_log());
    return sections;
}
} // namespace su::diagnostics
