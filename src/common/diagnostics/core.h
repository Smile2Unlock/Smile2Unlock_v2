#pragma once
#include "common/log/core.h"
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace su::diagnostics {
enum class ErrorCode { io, process, denied, invalid_path, invalid_result };
struct Error {
    ErrorCode code;
    std::string detail;
};
std::vector<su::log::Field> to_fields(const Error& error);
inline constexpr std::size_t output_limit = 4 * 1024 * 1024;
inline constexpr std::size_t log_limit = 8 * 1024 * 1024;
struct Record {
    std::string name;
    std::string value;
};
struct Section {
    std::string name;
    std::vector<Record> records;
};
struct Summary {
    std::string product_version, core_version, os, local_time;
    bool service_available = false;
    std::string service_reason, storage_protection;
    bool recognizer_available = false;
    std::vector<std::string> cameras;
    std::size_t profile_count = 0;
    std::vector<Section> targets;
    bool models_present = false;
    std::string model_dir;
    std::vector<Record> runtime;
};
std::string archive_name(std::string_view timestamp, bool windows, std::size_t collision = 0);
std::string available_archive_name(std::string_view timestamp, bool windows,
                                   const std::vector<std::string>& existing);
bool log_file_name(std::string_view name, bool app_only = false);
bool staging_child(std::string_view name);
// Lexical validation is platform-independent; the caller must also inspect open handles.
bool valid_helper_path(std::string_view root, std::string_view candidate);
std::string truncate_output(std::string_view text, std::size_t cap = output_limit);
std::string format_system(const std::vector<Section>& sections);
std::string format_summary(const Summary& input);
} // namespace su::diagnostics
