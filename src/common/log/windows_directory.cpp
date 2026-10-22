#include "windows_directory.h"
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <array>

namespace su::log {
namespace {
bool trusted_sid(PSID sid) noexcept {
    return sid && (IsWellKnownSid(sid, WinLocalSystemSid) ||
                   IsWellKnownSid(sid, WinBuiltinAdministratorsSid));
}
std::expected<void, SinkError> check_owner(HANDLE handle) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR raw = nullptr;
    const auto result = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                                        nullptr, nullptr, nullptr, &raw);
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    if (result != ERROR_SUCCESS || !trusted_sid(owner))
        return std::unexpected(SinkError::untrusted_owner);
    return {};
}
std::expected<void, SinkError> check_dacl(HANDLE handle) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR raw = nullptr;
    const auto result = GetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                        nullptr, &dacl, nullptr, &raw);
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    if (result != ERROR_SUCCESS || !dacl)
        return std::unexpected(SinkError::unsafe_permissions);
    for (auto index = DWORD{0}; index < dacl->AceCount; ++index) {
        void* raw_ace = nullptr;
        if (!GetAce(dacl, index, &raw_ace))
            return std::unexpected(SinkError::unsafe_permissions);
        const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
            !trusted_sid(const_cast<DWORD*>(&ace->SidStart)))
            return std::unexpected(SinkError::unsafe_permissions);
    }
    return {};
}
std::expected<void, SinkError> apply_dacl(HANDLE handle, PSECURITY_DESCRIPTOR descriptor) {
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present ||
        SetSecurityInfo(handle, SE_FILE_OBJECT,
                        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                        nullptr, dacl, nullptr) != ERROR_SUCCESS)
        return std::unexpected(SinkError::unsafe_permissions);
    return {};
}
} // namespace

std::expected<void, SinkError> check_handle(HANDLE handle, bool directory, bool owner) {
    auto info = FILE_ATTRIBUTE_TAG_INFO{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)))
        return std::unexpected(SinkError::open_failed);
    if (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return std::unexpected(SinkError::reparse_point);
    if (bool(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
        return std::unexpected(SinkError::open_failed);
    return owner ? check_owner(handle) : std::expected<void, SinkError>{};
}

std::expected<DirectoryHandles, SinkError> check_directory_chain(const std::filesystem::path& path,
                                                                 bool system) {
    auto handles = DirectoryHandles{};
    auto current = path.root_path();
    if (current.empty() || !path.is_absolute())
        return std::unexpected(SinkError::open_failed);
    auto product = false;
    for (const auto& component : path.relative_path()) {
        if (component == L".." || component == L".")
            return std::unexpected(SinkError::open_failed);
        current /= component;
        product = product || component == L"Smile2Unlock";
        auto handle =
            Handle{CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (handle.get() == INVALID_HANDLE_VALUE)
            return std::unexpected(SinkError::open_failed);
        if (const auto checked = check_handle(handle.get(), true, system && product); !checked)
            return std::unexpected(checked.error());
        if (system && product) {
            if (const auto checked = check_dacl(handle.get()); !checked)
                return std::unexpected(checked.error());
        }
        handles.push_back(std::move(handle));
    }
    return handles;
}

std::expected<void, SinkError> create_system_directory(const std::filesystem::path& path,
                                                       bool administrators) {
    // Keep ancestors open without FILE_SHARE_DELETE until ACL application completes.
    auto parents = check_directory_chain(path.parent_path(), false);
    if (!parents)
        return std::unexpected(parents.error());
    PSECURITY_DESCRIPTOR raw = nullptr;
    const auto sddl =
        administrators ? L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)" : L"D:P(A;OICI;FA;;;SY)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &raw, nullptr))
        return std::unexpected(SinkError::unsafe_permissions);
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    auto attributes = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
    if (!CreateDirectoryW(path.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
        return std::unexpected(SinkError::open_failed);
    const auto handle =
        Handle{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (handle.get() == INVALID_HANDLE_VALUE)
        return std::unexpected(SinkError::open_failed);
    if (const auto checked = check_handle(handle.get(), true, true); !checked)
        return checked;
    return apply_dacl(handle.get(), descriptor.get());
}

std::expected<std::filesystem::path, SinkError> program_data_path() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw)))
        return std::unexpected(SinkError::open_failed);
    const auto memory = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>{raw, &CoTaskMemFree};
    return std::filesystem::path{raw};
}
std::expected<void, SinkError> ensure_system_log_directory() {
    const auto root = program_data_path();
    if (!root)
        return std::unexpected(root.error());
    const auto product = *root / L"Smile2Unlock";
    if (const auto created = create_system_directory(product); !created)
        return created;
    return create_system_directory(product / L"Logs", true);
}
} // namespace su::log
