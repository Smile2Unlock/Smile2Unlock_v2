#pragma once
#ifdef _WIN32
#include "sinks.h"
#include <windows.h>
#include <memory>

namespace su::log {
struct CloseHandleDeleter {
    void operator()(void* handle) const noexcept {
        if (handle && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
using DirectoryHandles = std::vector<Handle>;
std::expected<DirectoryHandles, SinkError> check_directory_chain(const std::filesystem::path& path,
                                                                 bool system_directory);
std::expected<void, SinkError> check_handle(HANDLE handle, bool directory, bool trusted_owner);
// Existing untrusted objects are rejected before any security descriptor is changed.
std::expected<void, SinkError> create_system_directory(const std::filesystem::path& path,
                                                       bool administrators = false);
std::expected<std::filesystem::path, SinkError> program_data_path();
std::expected<void, SinkError> ensure_system_log_directory();
} // namespace su::log
#endif
