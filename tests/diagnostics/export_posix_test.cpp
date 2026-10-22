#include "common/diagnostics/export.h"
#include "common/diagnostics/process.h"
#include "common/log/logger.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
namespace d = su::diagnostics;
namespace fs = std::filesystem;
namespace {
int checks = 0;
void check(bool value) { ++checks; if (!value) { std::cerr << "check " << checks << " failed\n"; std::exit(1); } }
void write(const fs::path& path, std::string_view text) { std::ofstream{path} << text; }
void check_file_and_process_boundaries(const fs::path& root) {
    write(root / "large.log", std::string(d::log_limit + 128, 'x') + "TAIL");
    const auto tail = d::read_log_tail(root / "large.log");
    check(tail && tail->size() == d::log_limit && tail->ends_with("TAIL"));
    fs::create_symlink(root / "large.log", root / "symlink.log");
    check(!d::read_log_tail(root / "symlink.log"));
    check(!d::run_command({"echo", "unsafe"}));
    const auto bounded = d::run_command({"/usr/bin/printf", "%010000d", "1"}, 100);
    check(bounded && bounded->size() <= 100 && bounded->ends_with("[output truncated]\n"));
    check(!d::run_command({"/usr/bin/false"}));
    check(!d::run_command({"/usr/bin/sleep", "3"}, 100, std::chrono::seconds{1}));
    const auto first = d::reserve_archive(root / "reserved");
    check(first && *first);
    write(root / "reserved", "keep me");
    const auto again = d::reserve_archive(root / "reserved");
    check(again && !*again && fs::file_size(root / "reserved") == 7);
}
void check_bundle_contents(const fs::path& root, const fs::path& logs) {
    auto summary = d::Summary{}; summary.product_version = "test";
    const auto denied = d::export_bundle(summary, []() -> std::expected<d::PrivilegedData, d::Error> {
        return std::unexpected(d::Error{d::ErrorCode::denied, "declined test"});
    });
    check(denied && denied->system_unavailable == "declined test");
    const auto names = d::run_command({"/usr/bin/tar", "-tzf", denied->archive_path});
    check(names && names->find("./summary.txt") != names->npos && names->find("./app/app.log.1") != names->npos
        && names->find("./system/unavailable.txt") != names->npos && names->find("secrets.json") == names->npos);
    const auto reason = d::run_command({"/usr/bin/tar", "-xOzf", denied->archive_path, "./system/unavailable.txt"});
    check(reason && *reason == "declined test");
    const auto complete = d::export_bundle(summary, []() -> std::expected<d::PrivilegedData, d::Error> { return d::PrivilegedData{"system test", {}}; });
    check(complete && complete->system_unavailable.empty() && complete->archive_path != denied->archive_path);
    const auto text = d::run_command({"/usr/bin/tar", "-xOzf", complete->archive_path, "./system/system.txt"});
    check(text && *text == "system test");
    const auto privileged = root / "privileged";
    fs::create_directory(privileged); write(privileged / "system.txt", "snapshot");
    write(privileged / "service.log.1", "service tail");
    const auto partial = d::export_bundle(summary, [privileged]() -> std::expected<d::PrivilegedData, d::Error> {
        return d::PrivilegedData{{}, privileged, "one log unreadable"};
    });
    check(partial && partial->system_unavailable == "one log unreadable" && !fs::exists(privileged));
    const auto staged = d::run_command({"/usr/bin/tar", "-tzf", partial->archive_path});
    check(staged && staged->find("./system/system.txt") != staged->npos && staged->find("./system/service.log.1") != staged->npos);
    fs::create_directory(privileged); fs::create_symlink(logs / "app.log", privileged / "system.txt");
    const auto unsafe = d::export_bundle(summary, [privileged]() -> std::expected<d::PrivilegedData, d::Error> {
        return d::PrivilegedData{{}, privileged};
    });
    check(unsafe && !unsafe->system_unavailable.empty() && !fs::exists(privileged));
}

}
int main() {
    const auto root = fs::temp_directory_path() / ("s2u-export-test-" + std::to_string(getpid()));
    fs::remove_all(root); fs::create_directories(root / "state/smile2unlock/logs");
    fs::create_directory(root / "config"); fs::create_directory(root / "桌面");
    setenv("HOME", root.c_str(), 1); setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
    write(root / "config/user-dirs.dirs", "XDG_DESKTOP_DIR=\"$HOME/桌面\"\n");
    check(d::desktop_directory() == root / "桌面");
    check(su::log::app_log_directory() == root / "state/smile2unlock/logs");
    const auto logs = su::log::app_log_directory();
    write(logs / "app.log", "GUI log\n"); write(logs / "app.log.1", "rotated\n");
    write(logs / "secrets.json", "must not export");
    check_file_and_process_boundaries(root);
    check_bundle_contents(root, logs);
    fs::remove(root / "config/user-dirs.dirs");
    check(d::desktop_directory() == root);
    fs::create_directory(root / "Desktop"); check(d::desktop_directory() == root / "Desktop");
    fs::remove_all(root);
    std::cout << checks << " diagnostics export checks passed\n";
}
