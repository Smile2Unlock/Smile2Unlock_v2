#include "diagnostics.h"
#include "common/diagnostics/export.h"
#include "common/diagnostics/windows_security.h"
#include "common/log/windows_directory.h"
#include "common/log/logger.h"
#include "common/utf8_path.h"
#include <sddl.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <fstream>
#include <format>
namespace su::diagnostics {
namespace {
using su::log::Handle;
std::expected<std::wstring, Error> token_sid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return std::unexpected(Error{ErrorCode::denied, "token unavailable"});
    const auto token = Handle{raw};
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    auto bytes = std::vector<unsigned char>(size);
    if (!GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size)) return std::unexpected(Error{ErrorCode::denied, "token user unavailable"});
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text)) return std::unexpected(Error{ErrorCode::denied, "SID conversion failed"});
    const auto memory = std::unique_ptr<void, decltype(&LocalFree)>{text, &LocalFree};
    return std::wstring{text};
}
std::expected<std::filesystem::path, Error> create_child(const std::filesystem::path& root) {
    const auto sid = token_sid();
    if (!sid) return std::unexpected(sid.error());
    unsigned char random[8]{};
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return std::unexpected(Error{ErrorCode::io, "secure randomness unavailable"});
    auto name = std::string{};
    for (const auto byte : random) name += std::format("{:02x}", byte);
    // GR/GX + DELETE + FILE_DELETE_CHILD; inherited to the staged files.
    const auto sddl = L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x001300e9;;;" + *sid + L")";
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr))
        return std::unexpected(Error{ErrorCode::denied, "staging security descriptor failed"});
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    auto attributes = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
    const auto child = root / name;
    if (!CreateDirectoryW(child.c_str(), &attributes)) return std::unexpected(Error{ErrorCode::io, "staging creation failed"});
    return child;
}
std::expected<void, Error> remove_staging_tree(const std::filesystem::path& path) {
    try {
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            const auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) return std::unexpected(Error{ErrorCode::io, "stale file attributes unavailable"});
            const auto directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (directory && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                const auto removed = remove_staging_tree(entry.path());
                if (!removed) return removed;
            } else if (!(directory ? RemoveDirectoryW(entry.path().c_str()) : DeleteFileW(entry.path().c_str()))) {
                return std::unexpected(Error{ErrorCode::io, "could not delete stale diagnostics entry"});
            }
        }
        if (!RemoveDirectoryW(path.c_str())) return std::unexpected(Error{ErrorCode::io, "could not delete stale diagnostics directory"});
        return {};
    } catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::io, error.what()}); }
}
struct StageCleanup {
    std::filesystem::path path;
    bool keep = false;
    ~StageCleanup() {
        if (keep) return;
        const auto removed = remove_staging_tree(path);
        if (!removed) su::log::process_logger().warn("diagnostics.cleanup_failed", to_fields(removed.error()));
    }
};
void remove_stale(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!staging_child(su::path_utf8(entry.path().filename()))) continue;
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        // Children are created read-only for the caller: it cannot plant links.
        const auto handle = Handle{CreateFileW(entry.path().c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!su::log::check_handle(handle.get(), true, true)) continue;
        const auto removed = remove_staging_tree(entry.path());
        if (!removed) su::log::process_logger().warn("diagnostics.cleanup_failed", to_fields(removed.error()));
    }
}
std::expected<void, Error> write_stage(const std::filesystem::path& path, std::string_view text) {
    // Preserve the administrator owner as well as the caller's inherited ACL.
    // A caller-owned file could otherwise have its DACL rewritten by that caller.
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (GetNamedSecurityInfoW(path.parent_path().c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &raw) != ERROR_SUCCESS)
        return std::unexpected(Error{ErrorCode::denied, "staging security unavailable"});
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    auto attributes = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
    const auto file = Handle{CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    DWORD written = 0;
    if (file.get() == INVALID_HANDLE_VALUE || !WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
        || written != text.size()) return std::unexpected(Error{ErrorCode::io, "staging write failed"});
    return {};
}
std::expected<void, Error> stage_logs(const std::filesystem::path& root, const std::filesystem::path& child) {
    // An elevated administrator cannot read the SYSTEM-only product directory normally.
    // Backup opens bypass that DACL without weakening it; restore the token on scope exit.
    const auto backup = BackupPrivilege::acquire();
    if (!backup) return std::unexpected(backup.error());
    const auto logs = root.parent_path() / L"Smile2Unlock/Logs";
    const auto chain = su::log::check_directory_chain(logs, true);
    if (!chain) return std::unexpected(Error{ErrorCode::invalid_path, "system logs directory rejected"});
    const auto files = system_log_files(logs);
    if (!files) return std::unexpected(files.error());
    for (const auto& path : *files) {
        const auto tail = read_system_log_tail(path);
        if (!tail) return std::unexpected(tail.error());
        const auto written = write_stage(child / path.filename(), *tail);
        if (!written) return written;
    }
    return {};
}
}
std::expected<std::filesystem::path, Error> collect_windows_system() {
    try {
        const auto root = windows_diagnostics_root();
        if (!root) return std::unexpected(root.error());
        const auto created = create_protected_directory(*root);
        if (!created) return std::unexpected(created.error());
        const auto chain = open_protected_directory(*root);
        if (!chain) return std::unexpected(Error{ErrorCode::invalid_path, "diagnostics ancestors rejected"});
        remove_stale(*root);
        const auto child = create_child(*root);
        if (!child) return std::unexpected(child.error());
        auto cleanup = StageCleanup{*child};
        const auto child_guard = su::log::check_directory_chain(*child, false);
        if (!child_guard) return std::unexpected(Error{ErrorCode::invalid_path, "fresh staging directory rejected"});
        const auto logs = stage_logs(*root, *child);
        auto records = windows_system_records();
        if (!logs) {
            records.push_back({"System logs unavailable", {{"reason", logs.error().detail}}});
            const auto reason = write_stage(*child / "unavailable.txt", logs.error().detail);
            if (!reason) return std::unexpected(reason.error());
        }
        const auto written = write_stage(*child / "system.txt", format_system(records));
        if (!written) return std::unexpected(written.error());
        cleanup.keep = true;
        return child;
    } catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::io, error.what()}); }
}
}
