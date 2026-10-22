#include "export.h"
#include "common/log/logger.h"
#include "common/utf8_path.h"
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <iomanip>
namespace su::diagnostics {
namespace {
struct Staging {
    std::filesystem::path path;
    ~Staging() {
        if (path.empty()) return;
        auto error = std::error_code{};
        std::filesystem::remove_all(path, error);
        if (error) su::log::process_logger().warn("diagnostics.cleanup_failed",
            {su::log::field("path", su::path_utf8(path)), su::log::field("cause", error.message())});
    }
};
std::expected<std::filesystem::path, Error> temporary_directory() {
    const auto root = std::filesystem::temp_directory_path();
    auto random = std::random_device{};
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto name = std::ostringstream{};
        name << "smile2unlock-diagnostics-" << std::hex << random() << random();
        const auto path = root / name.str();
        if (std::filesystem::create_directory(path)) {
#ifndef _WIN32
            std::filesystem::permissions(path, std::filesystem::perms::owner_all);
#endif
            return path;
        }
    }
    return std::unexpected(Error{ErrorCode::io, "could not create a private staging directory"});
}
std::expected<void, Error> write_text(const std::filesystem::path& path, std::string_view text) {
    auto file = std::ofstream(path, std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    if (!file) return std::unexpected(Error{ErrorCode::io, "could not write " + su::path_utf8(path)});
    return {};
}
std::expected<void, Error> copy_logs(const std::filesystem::path& from,
                                    const std::filesystem::path& to, bool app) {
    if (!app && !std::filesystem::is_regular_file(from / "system.txt"))
        return std::unexpected(Error{ErrorCode::invalid_result, "helper did not produce system.txt"});
    auto error = std::error_code{};
    if (!std::filesystem::exists(from, error) && !error) return {};
    for (const auto& file : std::filesystem::directory_iterator(from)) {
        const auto name = su::path_utf8(file.path().filename());
        if (!(log_file_name(name, app) || (!app && name == "system.txt"))) continue;
        if (!std::filesystem::is_regular_file(file.symlink_status())) continue;
        const auto text = read_log_tail(file.path());
        if (!text) return std::unexpected(text.error());
        const auto written = write_text(to / file.path().filename(), *text);
        if (!written) return written;
    }
    return {};
}
std::pair<std::string, std::string> time_strings() {
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    auto local = std::tm{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::array<char, 64> timestamp{}, display{};
    std::strftime(timestamp.data(), timestamp.size(), "%Y%m%d-%H%M%S", &local);
    std::strftime(display.data(), display.size(), "%Y-%m-%dT%H:%M:%S%z", &local);
    return {timestamp.data(), display.data()};
}
std::expected<std::filesystem::path, Error> reserve_destination(std::string_view timestamp) {
    const auto desktop = desktop_directory();
    if (!desktop) return std::unexpected(desktop.error());
    for (std::size_t index = 0;; ++index) {
        const auto path = *desktop / archive_name(timestamp,
#ifdef _WIN32
            true,
#else
            false,
#endif
            index);
        const auto reserved = reserve_archive(path);
        if (!reserved) return std::unexpected(reserved.error());
        if (*reserved) return path;
    }
}
std::expected<std::string, Error> collect_system(const Collector& collect,
                                                const std::filesystem::path& destination) {
    auto data = collect();
    if (!data) return data.error().detail;
    if (!data->directory.empty()) {
        const auto staging = Staging{data->directory};
        const auto copied = copy_logs(staging.path, destination, false);
        if (!copied) return copied.error().detail;
    } else {
        const auto written = write_text(destination / "system.txt", truncate_output(data->text));
        if (!written) return std::unexpected(written.error());
    }
    return std::string{};
}
std::expected<ExportResult, Error> perform_export(Summary summary, const Collector& collect) {
    const auto temporary = temporary_directory();
    if (!temporary) return std::unexpected(temporary.error());
    const auto staging = Staging{*temporary};
    std::filesystem::create_directory(staging.path / "app");
    std::filesystem::create_directory(staging.path / "system");
    const auto [timestamp, local_time] = time_strings();
    summary.local_time = local_time;
    const auto os = operating_system();
    summary.os = os ? *os : os.error().detail;
    const auto written = write_text(staging.path / "summary.txt", format_summary(summary));
    if (!written) return std::unexpected(written.error());
    const auto copied = copy_logs(su::log::app_log_directory(), staging.path / "app", true);
    if (!copied) return std::unexpected(copied.error());
    const auto system = collect_system(collect, staging.path / "system");
    if (!system) return std::unexpected(system.error());
    if (!system->empty()) {
        su::log::process_logger().warn("diagnostics.privileged_unavailable", {su::log::field("cause", *system)});
        const auto unavailable = write_text(staging.path / "system/unavailable.txt", *system);
        if (!unavailable) return std::unexpected(unavailable.error());
    }
    const auto archive = reserve_destination(timestamp);
    if (!archive) return std::unexpected(archive.error());
    const auto packed = create_archive(staging.path, *archive);
    if (!packed) { std::error_code ignored; std::filesystem::remove(*archive, ignored); return std::unexpected(packed.error()); }
    su::log::process_logger().info("diagnostics.completed", {su::log::field("path", su::path_utf8(*archive)),
        su::log::field("size", std::filesystem::file_size(*archive))});
    return ExportResult{su::path_utf8(*archive), *system};
}
}
std::expected<ExportResult, Error> export_bundle(Summary summary, const Collector& collect) {
    su::log::process_logger().info("diagnostics.export_started");
    auto result = std::expected<ExportResult, Error>{std::unexpected(Error{ErrorCode::io, "export failed"})};
    try { result = perform_export(std::move(summary), collect); }
    catch (const std::exception& error) { result = std::unexpected(Error{ErrorCode::io, error.what()}); }
    if (!result) su::log::process_logger().error("diagnostics.failed", to_fields(result.error()));
    return result;
}
}
