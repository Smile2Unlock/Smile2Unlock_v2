#pragma once
#include "core.h"
#include <filesystem>
#include <functional>
namespace su::diagnostics {
struct PrivilegedData { std::string text; std::filesystem::path directory; };
struct ExportResult { std::string archive_path; std::string system_unavailable; };
using Collector = std::function<std::expected<PrivilegedData, Error>()>;
std::expected<ExportResult, Error> export_bundle(Summary summary, const Collector& collect);
std::expected<std::filesystem::path, Error> desktop_directory();
#ifdef _WIN32
std::expected<std::filesystem::path, Error> validate_windows_staging(std::string_view path);
#endif
std::expected<std::string, Error> operating_system();
std::expected<void, Error> create_archive(const std::filesystem::path& staging,
                                         const std::filesystem::path& archive);
std::expected<bool, Error> reserve_archive(const std::filesystem::path& path);
std::expected<std::string, Error> read_log_tail(const std::filesystem::path& path);
std::expected<void, Error> show_in_folder(std::string_view archive);
}
