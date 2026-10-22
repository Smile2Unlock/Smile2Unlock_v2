#include "common/diagnostics/export.h"
#include "common/log/windows_directory.h"
#include "common/utf8_path.h"
#include <fstream>
#include <iostream>
namespace d = su::diagnostics;
namespace fs = std::filesystem;
namespace {
int checks = 0;
void check(bool value) { ++checks; if (!value) { std::cerr << "check " << checks << " failed\n"; std::exit(1); } }
}
int main() {
    const auto root = fs::temp_directory_path() / (L"s2u-diag-测试-" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(root); fs::create_directory(root);
    const auto file = root / L"应用.log";
    const auto reserved = d::reserve_archive(file); check(reserved && *reserved);
    std::ofstream{file, std::ios::binary} << std::string(d::log_limit + 64, 'x') << "TAIL";
    const auto again = d::reserve_archive(file); check(again && !*again);
    const auto tail = d::read_log_tail(file);
    check(tail && tail->size() == d::log_limit && tail->ends_with("TAIL"));
    check(!d::read_log_tail(root));
    check(!d::validate_windows_staging(su::path_utf8(root)));
    check(!d::validate_windows_staging("C:/ProgramData/Smile2Unlock/Diagnostics/../0123456789abcdef"));
    const auto os = d::operating_system(); check(os && os->find("EditionID=") != os->npos && os->find("Build=") != os->npos);
    const auto desktop = d::desktop_directory(); check(desktop && desktop->is_absolute());
    const auto link = root / "linked.log";
    if (CreateSymbolicLinkW(link.c_str(), file.c_str(), 0)) check(!d::read_log_tail(link));
    fs::remove_all(root);
    std::cout << checks << " Windows diagnostics checks passed\n";
}
