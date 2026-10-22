#include "windows_security.h"
#include "common/log/logger.h"
#include "common/utf8_path.h"
#include <aclapi.h>
#include <array>
#include <sddl.h>

namespace su::diagnostics {
namespace {
Error win32_error(std::string_view operation) {
    return {ErrorCode::denied,
            std::string(operation) + " (Win32 " + std::to_string(GetLastError()) + ")"};
}

std::expected<void, Error> check_protected_dacl(HANDLE handle) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR raw = nullptr;
    const auto result = GetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                        nullptr, &dacl, nullptr, &raw);
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    if (result != ERROR_SUCCESS || !dacl)
        return std::unexpected(Error{ErrorCode::denied, "diagnostics root DACL unavailable"});
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* entry = nullptr;
        if (!GetAce(dacl, index, &entry))
            return std::unexpected(win32_error("read diagnostics root DACL"));
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
        const auto sid = const_cast<DWORD*>(&ace->SidStart);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
            !(IsWellKnownSid(sid, WinLocalSystemSid) ||
              IsWellKnownSid(sid, WinBuiltinAdministratorsSid)))
            return std::unexpected(Error{ErrorCode::denied, "diagnostics root DACL rejected"});
    }
    return {};
}

std::expected<std::filesystem::path, Error> final_path(HANDLE handle) {
    const auto count = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!count)
        return std::unexpected(win32_error("resolve staging handle path"));
    auto text = std::wstring(count, L'\0');
    const auto written =
        GetFinalPathNameByHandleW(handle, text.data(), count, FILE_NAME_NORMALIZED);
    if (!written || written >= count)
        return std::unexpected(win32_error("resolve staging handle path"));
    text.resize(written);
    if (text.starts_with(L"\\\\?\\UNC\\"))
        text = L"\\\\" + text.substr(8);
    else if (text.starts_with(L"\\\\?\\"))
        text.erase(0, 4);
    return std::filesystem::path{text};
}
void append_log_names(std::vector<std::filesystem::path>& files, const std::filesystem::path& path,
                      const void* buffer) {
    const auto* entry = static_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer);
    for (;;) {
        const auto name = std::filesystem::path{
            std::wstring_view{entry->FileName, entry->FileNameLength / sizeof(wchar_t)}};
        if (!(entry->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            log_file_name(su::path_utf8(name)))
            files.push_back(path / name);
        if (!entry->NextEntryOffset)
            break;
        entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
            reinterpret_cast<const unsigned char*>(entry) + entry->NextEntryOffset);
    }
}

} // namespace

BackupPrivilege::BackupPrivilege(su::log::Handle token, TOKEN_PRIVILEGES previous)
    : token_(std::move(token)), previous_(previous) {}

std::expected<BackupPrivilege, Error> BackupPrivilege::acquire() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &raw))
        return std::unexpected(win32_error("open backup privilege token"));
    auto token = su::log::Handle{raw};
    auto requested = TOKEN_PRIVILEGES{};
    requested.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, L"SeBackupPrivilege", &requested.Privileges[0].Luid))
        return std::unexpected(win32_error("resolve SeBackupPrivilege"));
    requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    auto previous = TOKEN_PRIVILEGES{};
    DWORD size = sizeof(previous);
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token.get(), FALSE, &requested, sizeof(previous), &previous,
                               &size) ||
        GetLastError() != ERROR_SUCCESS)
        return std::unexpected(win32_error("enable SeBackupPrivilege"));
    return BackupPrivilege{std::move(token), previous};
}

BackupPrivilege::~BackupPrivilege() {
    if (!token_)
        return;
    if (!AdjustTokenPrivileges(token_.get(), FALSE, &previous_, 0, nullptr, nullptr))
        su::log::process_logger().warn("diagnostics.privilege_restore_failed",
                                       to_fields(win32_error("restore SeBackupPrivilege")));
}

std::expected<std::filesystem::path, Error> windows_diagnostics_root() {
    const auto data = su::log::program_data_path();
    if (!data)
        return std::unexpected(Error{ErrorCode::io, "ProgramData unavailable"});
    return *data / L"Smile2Unlock-Diagnostics";
}

std::expected<void, Error> create_protected_directory(const std::filesystem::path& path) {
    const auto parents = su::log::check_directory_chain(path.parent_path(), false);
    if (!parents)
        return std::unexpected(Error{ErrorCode::invalid_path, "diagnostics ancestors rejected"});
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &raw, nullptr))
        return std::unexpected(win32_error("create diagnostics security descriptor"));
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    auto attributes = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
    if (!CreateDirectoryW(path.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
        return std::unexpected(win32_error("create diagnostics root"));
    // Existing roots must already be safe; never repair an untrusted directory in place.
    const auto checked = open_protected_directory(path);
    if (!checked)
        return std::unexpected(checked.error());
    return {};
}

std::expected<su::log::DirectoryHandles, Error>
open_protected_directory(const std::filesystem::path& path) {
    auto chain = su::log::check_directory_chain(path, false);
    if (!chain || chain->empty() || !su::log::check_handle(chain->back().get(), true, true))
        return std::unexpected(Error{ErrorCode::invalid_path, "diagnostics root rejected"});
    if (const auto checked = check_protected_dacl(chain->back().get()); !checked)
        return std::unexpected(checked.error());
    return std::move(*chain);
}

namespace {
std::expected<su::log::Handle, Error> open_staging_child(const std::filesystem::path& root,
                                                         std::string_view candidate) {
    if (!valid_helper_path(su::path_utf8(root), candidate))
        return std::unexpected(Error{ErrorCode::invalid_path, "helper staging path rejected"});
    const auto path = su::path_from_utf8(candidate);
    auto handle = su::log::Handle{
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!su::log::check_handle(handle.get(), true, true))
        return std::unexpected(Error{ErrorCode::invalid_path, "helper staging directory rejected"});
    // The SY/BA-only parent intentionally denies an unelevated GUI READ_CONTROL.
    // Validate the opened child's normalized path instead of requesting access to that parent.
    const auto resolved = final_path(handle.get());
    if (!resolved || !valid_helper_path(su::path_utf8(root), su::path_utf8(*resolved)))
        return std::unexpected(
            Error{ErrorCode::invalid_path, "helper staging ancestors redirected"});
    return handle;
}
} // namespace

std::expected<std::filesystem::path, Error>
validate_staging_child(const std::filesystem::path& root, std::string_view candidate) {
    const auto handle = open_staging_child(root, candidate);
    if (!handle)
        return std::unexpected(handle.error());
    return su::path_from_utf8(candidate);
}

std::expected<su::log::DirectoryHandles, Error>
open_log_directory(const std::filesystem::path& path, bool system) {
    const auto root = windows_diagnostics_root();
    if (!system && root && valid_helper_path(su::path_utf8(*root), su::path_utf8(path))) {
        auto child = open_staging_child(*root, su::path_utf8(path));
        if (!child)
            return std::unexpected(child.error());
        auto handles = su::log::DirectoryHandles{};
        handles.push_back(std::move(*child));
        return handles;
    }
    auto chain = su::log::check_directory_chain(path, system);
    if (!chain)
        return std::unexpected(Error{ErrorCode::invalid_path, "log directory rejected"});
    return std::move(*chain);
}
std::expected<std::vector<std::filesystem::path>, Error>
system_log_files(const std::filesystem::path& directory) {
    const auto chain = su::log::check_directory_chain(directory, true);
    if (!chain)
        return std::unexpected(Error{ErrorCode::invalid_path, "system log ancestors rejected"});
    const auto handle = su::log::Handle{
        CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!su::log::check_handle(handle.get(), true, true))
        return std::unexpected(Error{ErrorCode::invalid_path, "system log directory rejected"});
    auto files = std::vector<std::filesystem::path>{};
    alignas(FILE_ID_BOTH_DIR_INFO) auto buffer = std::array<unsigned char, 65536>{};
    auto mode = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(handle.get(), mode, buffer.data(), buffer.size())) {
            if (GetLastError() == ERROR_NO_MORE_FILES)
                return files;
            return std::unexpected(win32_error("enumerate system log directory"));
        }
        append_log_names(files, directory, buffer.data());
        mode = FileIdBothDirectoryInfo;
    }
}
} // namespace su::diagnostics
