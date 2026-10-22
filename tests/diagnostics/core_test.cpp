#include "common/diagnostics/core.h"
#include <cstdlib>
#include <iostream>
using namespace su::diagnostics;
namespace {
int checks = 0;
void check(bool value) { ++checks; if (!value) { std::cerr << "check " << checks << " failed\n"; std::exit(1); } }
}
int main() {
    check(archive_name("20261022-120000", true) == "smile2unlock-diagnostics-20261022-120000.zip");
    check(archive_name("20261022-120000", false, 2) == "smile2unlock-diagnostics-20261022-120000-2.tar.gz");
    check(available_archive_name("x", true, {archive_name("x", true), archive_name("x", true, 1)}) == archive_name("x", true, 2));
    for (const auto* name : {"app.log", "app.log.1", "app.log.123"}) check(log_file_name(name, true));
    for (const auto* name : {"auth-service.log", "cp.log.2"}) check(log_file_name(name) && !log_file_name(name, true));
    for (const auto* name : {"app.log.", "app.log.x", "../app.log", "x\\app.log", ".log", "app.log.1.exe", "app.log:stream"}) check(!log_file_name(name));
    check(valid_helper_path("C:\\ProgramData\\Smile2Unlock\\Diagnostics", "c:/programdata/smile2unlock/diagnostics/0123456789abcdef"));
    for (const auto* path : {"C:/root/../0123456789abcdef", "C:/root/0123456789abcdef/x", "C:/root2/0123456789abcdef", "C:/root/../../Windows", "C:/root/0123456789abcdeg", "C:/root/0123456789abcdef:ads"}) check(!valid_helper_path("C:/root", path));
    check(truncate_output("abc", 3) == "abc");
    check(truncate_output(std::string(100, 'x'), 32) == std::string(12, 'x') + "\n[output truncated]\n");
    check(truncate_output("abcdef", 2).size() == 2);
    check(truncate_output(std::string(100, 'x'), 0).empty());
    const auto system = std::vector<Section>{{"Registry", {{"key", "missing"}}}, {"Service", {{"state", "running"}}}, {"Files", {{"a.dll", "size=4 sha256=abcd signature=valid"}}}, {"Events", {{"1000", "LogonUI"}}}};
    check(format_system(system) == "[Registry]\nkey: missing\n\n[Service]\nstate: running\n\n[Files]\na.dll: size=4 sha256=abcd signature=valid\n\n[Events]\n1000: LogonUI\n\n");
    Summary summary;
    summary.product_version = "2.3"; summary.core_version = "1"; summary.os = "Linux";
    summary.local_time = "2026-10-22T12:00:00-0700"; summary.profile_count = 7;
    summary.cameras = {"IR camera"}; summary.targets = {{"PAM sddm", {{"state", "managed"}, {"path", "/etc/pam.d/sddm"}}}};
    const auto text = format_summary(summary);
    check(text.find("Profile count: 7\n") != text.npos);
    check(text.find("Camera count: 1\nCamera") == text.npos);
    check(text.find("Camera: IR camera\n") != text.npos);
    check(text.find("state: managed\npath: /etc/pam.d/sddm") != text.npos);
    check(text.find("Local time: 2026-10-22T12:00:00-0700") != text.npos);
    check(text.find("username") == text.npos && text.find("label") == text.npos);
    std::cout << checks << " diagnostics core checks passed\n";
}
