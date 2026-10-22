#include "diagnostics.h"
#include "common/diagnostics/process.h"
#include <fstream>
#include <filesystem>
namespace su::diagnostics {
namespace {
Section command_section(std::string name, std::vector<std::string> command) {
    const auto result = run_command(command);
    return {std::move(name), {{"output", result ? *result : result.error().detail}}};
}
Section pam_lines() {
    auto section = Section{"PAM configuration", {}};
    auto error = std::error_code{};
    for (const auto& file : std::filesystem::directory_iterator("/etc/pam.d", error)) {
        if (!file.is_regular_file(error) || file.is_symlink(error)) continue;
        auto input = std::ifstream(file.path());
        std::string line;
        std::size_t number = 0, bytes = 0;
        while (bytes < output_limit && std::getline(input, line)) {
            ++number; bytes += line.size();
            if (line.find("smile2unlock") != line.npos)
                section.records.push_back({file.path().string() + ":" + std::to_string(number), truncate_output(line, 8192)});
        }
    }
    if (error) section.records.push_back({"error", error.message()});
    return section;
}
}
std::expected<std::string, Error> collect_linux_system() {
    auto sections = std::vector<Section>{};
    for (const auto* unit : {"su-authd.service", "su-deploy-helper.service"}) {
        sections.push_back(command_section(std::string("Journal ") + unit,
            {"/usr/bin/journalctl", "--no-pager", "-o", "short-iso", "-n", "2000", "-u", unit}));
        sections.push_back(command_section(std::string("Service ") + unit,
            {"/usr/bin/systemctl", "show", "-p", "ActiveState,SubState,ExecMainStatus,NRestarts", unit}));
    }
    // pam_sink uses pam_syslog: the identifier belongs to the PAM host (sddm,
    // login, etc.), not a fixed module identifier. Match the module in MESSAGE.
    sections.push_back(command_section("PAM journal (pam_syslog host identifiers)",
        {"/usr/bin/journalctl", "--no-pager", "-o", "short-iso", "-n", "2000", "--grep=smile2unlock|Smile2Unlock"}));
    sections.push_back(pam_lines());
    return format_system(sections);
}
}
