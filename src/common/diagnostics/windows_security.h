#pragma once
#include "core.h"
#include "common/log/windows_directory.h"

namespace su::diagnostics {
// Only the helper enables this privilege; its previous token state is restored on scope exit.
class BackupPrivilege {
public:
    static std::expected<BackupPrivilege, Error> acquire();
    BackupPrivilege(BackupPrivilege&&) noexcept = default;
    BackupPrivilege& operator=(BackupPrivilege&&) = delete;
    ~BackupPrivilege();

private:
    BackupPrivilege(su::log::Handle token, TOKEN_PRIVILEGES previous);
    su::log::Handle token_;
    TOKEN_PRIVILEGES previous_{};
};

std::expected<std::filesystem::path, Error> windows_diagnostics_root();
std::expected<void, Error> create_protected_directory(const std::filesystem::path& path);
std::expected<su::log::DirectoryHandles, Error>
open_protected_directory(const std::filesystem::path& path);
std::expected<std::filesystem::path, Error>
validate_staging_child(const std::filesystem::path& root, std::string_view candidate);
std::expected<su::log::DirectoryHandles, Error>
open_log_directory(const std::filesystem::path& path, bool system);
std::expected<std::vector<std::filesystem::path>, Error>
system_log_files(const std::filesystem::path& directory);
std::expected<std::string, Error> read_system_log_tail(const std::filesystem::path& path);
} // namespace su::diagnostics
