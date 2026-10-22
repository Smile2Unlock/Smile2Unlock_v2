#include "diagnostics.h"
#include "common/diagnostics/export.h"
#include "common/log/windows_directory.h"
#include "common/log/logger.h"
#include "common/utf8_path.h"
#include <sddl.h>
#include <bcrypt.h>
#include <fstream>
#include <format>
namespace su::diagnostics {
namespace {
using su::log::Handle;
std::expected<std::filesystem::path, Error> diagnostics_root() {
    const auto data = su::log::program_data_path();
    if (!data) return std::unexpected(Error{ErrorCode::io, "ProgramData unavailable"});
    return *data / L"Smile2Unlock/Diagnostics";
}
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
        auto error = std::error_code{};
        std::filesystem::remove_all(entry.path(), error);
        if (error) su::log::process_logger().warn("diagnostics.cleanup_failed", {su::log::field("cause", error.message())});
    }
}
std::expected<void, Error> write_stage(const std::filesystem::path& path, std::string_view text) {
    const auto file = Handle{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    DWORD written = 0;
    if (file.get() == INVALID_HANDLE_VALUE || !WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
        || written != text.size()) return std::unexpected(Error{ErrorCode::io, "staging write failed"});
    return {};
}
std::expected<void, Error> stage_logs(const std::filesystem::path& root, const std::filesystem::path& child) {
    const auto logs = root.parent_path() / "Logs";
    if (!std::filesystem::exists(logs)) return {};
    const auto chain = su::log::check_directory_chain(logs, true);
    if (!chain) return std::unexpected(Error{ErrorCode::invalid_path, "system logs directory rejected"});
    for (const auto& entry : std::filesystem::directory_iterator(logs)) {
        if (!log_file_name(su::path_utf8(entry.path().filename()))) continue;
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) continue;
        const auto tail = read_log_tail(entry.path());
        if (!tail) return std::unexpected(tail.error());
        const auto written = write_stage(child / entry.path().filename(), *tail);
        if (!written) return written;
    }
    return {};
}
}
std::expected<std::filesystem::path, Error> collect_windows_system() {
    try {
        const auto root = diagnostics_root();
        if (!root) return std::unexpected(root.error());
        const auto product = su::log::create_system_directory(root->parent_path(), true);
        if (!product || !su::log::create_system_directory(*root, true)) return std::unexpected(Error{ErrorCode::denied, "protected diagnostics root rejected"});
        const auto chain = su::log::check_directory_chain(*root, true);
        if (!chain) return std::unexpected(Error{ErrorCode::invalid_path, "diagnostics ancestors rejected"});
        remove_stale(*root);
        const auto child = create_child(*root);
        if (!child) return std::unexpected(child.error());
        const auto logs = stage_logs(*root, *child);
        auto records = windows_system_records();
        if (!logs) records.push_back({"System logs unavailable", {{"reason", logs.error().detail}}});
        const auto written = write_stage(*child / "system.txt", format_system(records));
        if (!written) { std::filesystem::remove_all(*child); return std::unexpected(written.error()); }
        return child;
    } catch (const std::exception& error) { return std::unexpected(Error{ErrorCode::io, error.what()}); }
}
}
