#pragma once
#include "core.h"
#include <expected>
#include <filesystem>
#include <functional>

namespace su::log {
enum class SinkError {
    open_failed,
    reparse_point,
    untrusted_owner,
    unsafe_permissions,
    write_failed,
    rotate_failed
};
using Sink = std::function<std::expected<void, SinkError>(Level, std::string_view)>;
Sink stderr_sink();
Sink rotating_file_sink(std::filesystem::path path, RotationPolicy policy = {},
                        bool system_directory = false);
Sink fallback(Sink primary, Sink secondary);
Sink tee(Sink first, Sink second);
std::vector<Field> to_fields(SinkError error);
#ifdef _WIN32
Sink debugger_sink();
std::vector<Field> win32_error_fields(unsigned long error);
inline std::vector<Field> to_fields(unsigned long error) { return win32_error_fields(error); }
#else
Sink syslog_sink(std::string ident, int facility);
#endif
} // namespace su::log
