#include "noctalia.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <ranges>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

namespace su::deploy {
namespace {

constexpr std::string_view kHelperName = "noctalia-pam-helper";
constexpr std::string_view kShellName = "noctalia";
constexpr std::string_view kDefaultService = "login";
constexpr std::size_t kMaxServiceNameLength = 64;
// Real helpers are a few MiB; refuse anything absurd rather than slurp it.
constexpr std::uintmax_t kMaxHelperBytes = 64U * 1024U * 1024U;
constexpr std::array kFixedPrefixes{std::string_view{"/usr"}, std::string_view{"/usr/local"}};
// Shared stacks that happen to be spelled in many binaries' string tables.
constexpr std::array kGenericServices{
    std::string_view{"other"},        std::string_view{"system-auth"},
    std::string_view{"password-auth"}, std::string_view{"common-auth"},
    std::string_view{"system-login"}, std::string_view{"system-local-login"},
    std::string_view{"postlogin"},
};

bool contains_c_string(std::string_view binary, std::string_view name) {
    for (auto at = binary.find(name); at != std::string_view::npos;
         at = binary.find(name, at + 1)) {
        const auto before_ok = at == 0 || binary[at - 1] == '\0';
        const auto end = at + name.size();
        const auto after_ok = end == binary.size() || binary[end] == '\0';
        if (before_ok && after_ok) {
            return true;
        }
    }
    return false;
}

bool is_executable_file(const std::filesystem::path& path) {
    struct stat status {};
    return ::stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode)
        && (status.st_mode & S_IXUSR) != 0;
}

std::filesystem::path rooted(const std::filesystem::path& root, std::string_view absolute) {
    return root / std::filesystem::path(absolute).relative_path();
}

std::optional<std::string> read_limited(const std::filesystem::path& path) {
    auto error = std::error_code{};
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaxHelperBytes) {
        return std::nullopt;
    }
    auto input = std::ifstream(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Same search order as upstream: explicit override, next to the shell, then
// the install prefixes.
std::vector<std::filesystem::path> helper_candidates(
    const std::filesystem::path& root, const NoctaliaSearch& search, bool& shell_found) {
    auto candidates = std::vector<std::filesystem::path>{};
    if (search.helper_override) {
        candidates.push_back(*search.helper_override);
    }
    for (const auto& directory : search.path_directories) {
        if (is_executable_file(directory / kShellName)) {
            shell_found = true;
            candidates.push_back(directory / kHelperName);
            candidates.push_back(directory / ".." / "libexec" / "noctalia" / kHelperName);
        }
    }
    for (const auto prefix : kFixedPrefixes) {
        candidates.push_back(rooted(root, prefix) / "libexec" / "noctalia" / kHelperName);
    }
    return candidates;
}

std::vector<std::string> installed_service_names(const std::filesystem::path& root) {
    auto names = std::vector<std::string>{};
    auto error = std::error_code{};
    for (const auto& entry :
         std::filesystem::directory_iterator(rooted(root, "/etc/pam.d"), error)) {
        auto entry_error = std::error_code{};
        if (!entry.is_regular_file(entry_error) || entry_error) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (is_valid_pam_service_name(name) && name != kDefaultService
            && !std::ranges::contains(kGenericServices, name)) {
            names.push_back(name);
        }
    }
    std::ranges::sort(names);
    return names;
}

} // namespace

bool is_valid_pam_service_name(std::string_view name) {
    return !name.empty() && name.size() <= kMaxServiceNameLength && name.front() != '.'
        && std::ranges::all_of(name, [](unsigned char c) {
               return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
           });
}

NoctaliaScanResult scan_compiled_pam_service(
    std::string_view binary, std::span<const std::string> candidates) {
    const auto matches = candidates
        | std::views::filter([&](const std::string& name) {
              return name != kDefaultService && contains_c_string(binary, name);
          })
        | std::ranges::to<std::vector<std::string>>();
    // A service named after Noctalia is the strongest signal; other names are
    // only trusted when they are the sole match.
    const auto branded = matches
        | std::views::filter([](const std::string& name) { return name.contains("noctalia"); })
        | std::ranges::to<std::vector<std::string>>();
    const auto& pool = branded.empty() ? matches : branded;
    if (pool.empty()) {
        return {.outcome = NoctaliaScan::kLogin, .service = std::string(kDefaultService)};
    }
    if (pool.size() > 1) {
        return {.outcome = NoctaliaScan::kAmbiguous, .service = {}};
    }
    return {.outcome = NoctaliaScan::kService, .service = pool.front()};
}

NoctaliaSearch noctalia_search_from_environment() {
    auto search = NoctaliaSearch{};
    if (const auto* override_path = std::getenv("NOCTALIA_PAM_HELPER_PATH");
        override_path != nullptr && override_path[0] != '\0') {
        search.helper_override = std::filesystem::path(override_path);
    }
    if (const auto* path = std::getenv("PATH"); path != nullptr) {
        for (const auto part : std::string_view(path) | std::views::split(':')) {
            const auto directory = std::string_view(part);
            if (!directory.empty()) {
                search.path_directories.emplace_back(directory);
            }
        }
    }
    return search;
}

NoctaliaDetection detect_noctalia(
    const std::filesystem::path& root, const NoctaliaSearch& search) {
    auto shell_found = false;
    const auto candidates = helper_candidates(root, search, shell_found);
    const auto helper = std::ranges::find_if(candidates, is_executable_file);
    if (helper == candidates.end()) {
        return {.install = shell_found ? NoctaliaInstall::kNoHelper : NoctaliaInstall::kAbsent};
    }
    const auto binary = read_limited(*helper);
    if (!binary) {
        return {.install = NoctaliaInstall::kUnreadable, .helper = *helper};
    }
    return {
        .install = NoctaliaInstall::kHelperFound,
        .helper = *helper,
        .scan = scan_compiled_pam_service(*binary, installed_service_names(root)),
    };
}

} // namespace su::deploy
