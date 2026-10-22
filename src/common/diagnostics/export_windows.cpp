#include "export.h"
#include "windows_security.h"
#include "common/log/windows_directory.h"
#include "common/utf8_path.h"
#include <shlobj.h>
#include <shellapi.h>
#include <format>
namespace su::diagnostics {
namespace {
std::expected<std::filesystem::path, Error> folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) return std::unexpected(Error{ErrorCode::io, "known folder unavailable"});
    const auto memory = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>{raw, &CoTaskMemFree};
    return std::filesystem::path{raw};
}
std::string registry_value(const wchar_t* name) {
    wchar_t value[512]{};
    DWORD bytes = sizeof(value), number = 0;
    constexpr auto key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value, &bytes) == ERROR_SUCCESS)
        return su::path_utf8(std::filesystem::path{value});
    bytes = sizeof(number);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &number, &bytes) == ERROR_SUCCESS)
        return std::to_string(number);
    return "unavailable";
}
}
std::expected<std::filesystem::path, Error> validate_windows_staging(std::string_view path) {
    const auto root = windows_diagnostics_root();
    if (!root) return std::unexpected(root.error());
    return validate_staging_child(*root, path);
}
std::expected<std::filesystem::path, Error> desktop_directory() { return folder(FOLDERID_Desktop); }
std::expected<std::string, Error> operating_system() {
    return registry_value(L"ProductName") + " EditionID=" + registry_value(L"EditionID")
        + " DisplayVersion=" + registry_value(L"DisplayVersion") + " Build="
        + registry_value(L"CurrentBuild") + "." + registry_value(L"UBR");
}
std::expected<bool, Error> reserve_archive(const std::filesystem::path& path) {
    const auto handle = su::log::Handle{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (handle.get() != INVALID_HANDLE_VALUE) return true;
    if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) return false;
    return std::unexpected(Error{ErrorCode::io, "could not reserve archive (Win32 " + std::to_string(GetLastError()) + ")"});
}
namespace {
std::expected<std::string, Error> read_tail(const std::filesystem::path& path, bool system) {
    auto parents = open_log_directory(path.parent_path(), system);
    if (!parents) return std::unexpected(Error{ErrorCode::invalid_path, "log directory rejected"});
    const auto file = su::log::Handle{CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (file.get() == INVALID_HANDLE_VALUE || !su::log::check_handle(file.get(), false, system))
        return std::unexpected(Error{ErrorCode::invalid_path, "log file rejected"});
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) return std::unexpected(Error{ErrorCode::io, "log size unavailable"});
    if (size.QuadPart > static_cast<LONGLONG>(log_limit)) {
        LARGE_INTEGER offset{}; offset.QuadPart = size.QuadPart - log_limit;
        if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN)) return std::unexpected(Error{ErrorCode::io, "log seek failed"});
    }
    auto text = std::string(std::min<std::size_t>(size.QuadPart, log_limit), '\0');
    DWORD count = 0;
    if (!ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &count, nullptr))
        return std::unexpected(Error{ErrorCode::io, "log read failed"});
    text.resize(count); return text;
}
} // namespace
std::expected<std::string, Error> read_log_tail(const std::filesystem::path& path) {
    return read_tail(path, false);
}
std::expected<std::string, Error> read_system_log_tail(const std::filesystem::path& path) {
    return read_tail(path, true);
}
std::expected<void, Error> create_archive(const std::filesystem::path& staging, const std::filesystem::path& archive) {
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) return std::unexpected(Error{ErrorCode::process, "system directory unavailable"});
    const auto executable = std::filesystem::path{system} / L"tar.exe";
    auto command = std::format(L"\"{}\" -a -c -f \"{}\" -C \"{}\" .", executable.wstring(), archive.wstring(), staging.wstring());
    auto startup = STARTUPINFOW{}; startup.cb = sizeof(startup);
    auto process = PROCESS_INFORMATION{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return std::unexpected(Error{ErrorCode::process, "could not launch system tar.exe"});
    const auto handle = su::log::Handle{process.hProcess};
    const auto thread = su::log::Handle{process.hThread};
    if (WaitForSingleObject(handle.get(), 120000) != WAIT_OBJECT_0) {
        TerminateProcess(handle.get(), 1); WaitForSingleObject(handle.get(), 5000);
        return std::unexpected(Error{ErrorCode::process, "archive creation timed out"});
    }
    DWORD code = 1;
    if (!GetExitCodeProcess(handle.get(), &code) || code != 0) return std::unexpected(Error{ErrorCode::process, "archive creation failed"});
    return {};
}
std::expected<void, Error> show_in_folder(std::string_view archive) {
    const auto path = su::path_from_utf8(archive);
    PIDLIST_ABSOLUTE item = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr)))
        return std::unexpected(Error{ErrorCode::io, "could not locate archive in Explorer"});
    const auto memory = std::unique_ptr<ITEMIDLIST, decltype(&CoTaskMemFree)>{item, &CoTaskMemFree};
    if (FAILED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0))) return std::unexpected(Error{ErrorCode::io, "could not open Explorer"});
    return {};
}
}
