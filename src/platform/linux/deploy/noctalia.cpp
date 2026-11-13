#include "noctalia.h"

#include "platform/linux/deploy/safe_read.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <mutex>
#include <ranges>
#include <iterator>
#include <system_error>
#include <type_traits>
#include <variant>

#include <sys/stat.h>

namespace su::deploy {
namespace {

constexpr std::string_view kHelperName = "noctalia-pam-helper";
constexpr std::string_view kShellName = "noctalia";
constexpr std::string_view kBrand = "noctalia";
constexpr std::string_view kDefaultService = "login";
// Real helpers are a few MiB; refuse anything absurd rather than slurp it.
constexpr std::size_t kMaxHelperBytes = 64U * 1024U * 1024U;
// PAM service files are tiny; this only bounds a hostile one.
constexpr std::size_t kMaxServiceFileBytes = 64U * 1024U;
constexpr std::size_t kMaxCachedHelpers = 8;
constexpr std::array kPamDirectories{std::string_view{"/etc/pam.d"},
                                     std::string_view{"/usr/lib/pam.d"}};
constexpr std::array kFixedPrefixes{std::string_view{"/usr"}, std::string_view{"/usr/local"}};
constexpr auto kUntrustedModeBits = mode_t{S_IWGRP | S_IWOTH};

std::filesystem::path rooted(const std::filesystem::path& root, std::string_view absolute) {
    return root / std::filesystem::path(absolute).relative_path();
}

bool is_executable_file(const std::filesystem::path& path) {
    struct stat status {};
    return ::stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode)
        && (status.st_mode & S_IXUSR) != 0;
}

// --- Helper discovery -------------------------------------------------------

struct Candidates {
    std::vector<std::filesystem::path> helpers;
    bool shell_found = false;
    bool require_trusted_owner = false;
    uid_t trusted_owner = 0;
};

std::vector<std::filesystem::path> fixed_helper_paths(const std::filesystem::path& root) {
    return kFixedPrefixes
        | std::views::transform([&](std::string_view prefix) {
              return rooted(root, prefix) / "libexec" / "noctalia" / kHelperName;
          })
        | std::ranges::to<std::vector>();
}

// Root mode: nothing here may come from the caller's environment.
Candidates root_candidates(const std::filesystem::path& root, const RootHelperLookup& lookup) {
    auto found = Candidates{.helpers = fixed_helper_paths(root),
                            .require_trusted_owner = true,
                            .trusted_owner = lookup.trusted_owner};
    found.shell_found = std::ranges::any_of(kFixedPrefixes, [&](std::string_view prefix) {
        return is_executable_file(rooted(root, prefix) / "bin" / kShellName);
    });
    return found;
}

// Same search order as upstream: explicit override, next to the shell, then
// the install prefixes.
Candidates environment_candidates(const std::filesystem::path& root, const NoctaliaSearch& search) {
    auto found = Candidates{};
    if (search.helper_override) {
        found.helpers.push_back(*search.helper_override);
    }
    for (const auto& directory : search.path_directories) {
        if (is_executable_file(directory / kShellName)) {
            found.shell_found = true;
            found.helpers.push_back(directory / kHelperName);
            found.helpers.push_back(directory / ".." / "libexec" / "noctalia" / kHelperName);
        }
    }
    std::ranges::copy(fixed_helper_paths(root), std::back_inserter(found.helpers));
    return found;
}

// --- Installed service names ------------------------------------------------

std::optional<std::string> read_service_file(const std::filesystem::path& path) {
    const auto opened = open_regular_nofollow(path);
    if (!opened) {
        return std::nullopt;
    }
    auto content = read_capped(*opened, kMaxServiceFileBytes);
    return content ? std::optional<std::string>{std::move(*content)} : std::nullopt;
}

// /etc/pam.d overrides the vendor copy in /usr/lib/pam.d, as in
// effective_service_path(); the winning file is the one that gets judged.
std::optional<std::filesystem::path> effective_path(
    const std::filesystem::path& root, std::string_view name) {
    for (const auto directory : kPamDirectories) {
        const auto candidate = rooted(root, directory) / name;
        auto error = std::error_code{};
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::vector<std::string> pam_file_names(const std::filesystem::path& root) {
    auto names = std::vector<std::string>{};
    for (const auto directory : kPamDirectories) {
        auto error = std::error_code{};
        for (const auto& entry :
             std::filesystem::directory_iterator(rooted(root, directory), error)) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::ranges::sort(names);
    const auto duplicates = std::ranges::unique(names);
    names.erase(duplicates.begin(), duplicates.end());
    return names;
}

std::vector<std::string> installed_service_names(
    const std::filesystem::path& root, const ServicePolicy& policy) {
    return pam_file_names(root)
        | std::views::filter([&](const std::string& name) {
              if (!check_noctalia_service_name(name, policy)) {
                  return false;
              }
              const auto path = effective_path(root, name);
              const auto content = path ? read_service_file(*path) : std::nullopt;
              return content && check_noctalia_service(name, *content, policy).has_value();
          })
        | std::ranges::to<std::vector>();
}

// --- Embedded names, cached per helper identity -----------------------------

struct HelperIdentity {
    dev_t device = 0;
    ino_t inode = 0;
    off_t size = 0;
    timespec modified{};
    bool operator==(const HelperIdentity& other) const {
        return device == other.device && inode == other.inode && size == other.size
            && modified.tv_sec == other.modified.tv_sec
            && modified.tv_nsec == other.modified.tv_nsec;
    }
};

HelperIdentity identity_of(const struct stat& status) {
    return {status.st_dev, status.st_ino, status.st_size, status.st_mtim};
}

struct CacheEntry {
    HelperIdentity identity;
    std::vector<std::string> embedded;
};

// Inspect is unprivileged and callable repeatedly; rescanning a multi-MiB
// binary each time would be a cheap way to burn the helper's time.
struct Cache {
    std::mutex lock;
    std::map<std::filesystem::path, CacheEntry> entries;
    NoctaliaCacheStats stats;
};

Cache& cache() {
    static Cache instance;
    return instance;
}

// Every NUL-delimited token that could be a Noctalia-branded service name.
std::vector<std::string> embedded_branded_names(std::string_view binary) {
    auto names = binary
        | std::views::split('\0')
        | std::views::transform([](auto part) { return std::string_view(part); })
        | std::views::filter([](std::string_view token) {
              return token.contains(kBrand) && is_valid_pam_service_name(token);
          })
        | std::ranges::to<std::vector<std::string>>();
    std::ranges::sort(names);
    const auto duplicates = std::ranges::unique(names);
    names.erase(duplicates.begin(), duplicates.end());
    return names;
}

std::expected<std::vector<std::string>, ReadFailure> embedded_names_cached(
    const std::filesystem::path& path, const OpenedFile& file) {
    const auto identity = identity_of(file.metadata());
    auto& state = cache();
    {
        const auto guard = std::scoped_lock{state.lock};
        if (const auto found = state.entries.find(path);
            found != state.entries.end() && found->second.identity == identity) {
            ++state.stats.hits;
            return found->second.embedded;
        }
    }
    const auto binary = read_capped(file, kMaxHelperBytes);
    if (!binary) {
        return std::unexpected(binary.error());
    }
    auto names = embedded_branded_names(*binary);
    const auto guard = std::scoped_lock{state.lock};
    ++state.stats.misses;
    if (state.entries.size() >= kMaxCachedHelpers && !state.entries.contains(path)) {
        state.entries.clear();
    }
    state.entries.insert_or_assign(path, CacheEntry{identity, names});
    return names;
}

NoctaliaScanResult select_service(
    std::span<const std::string> embedded, std::span<const std::string> candidates) {
    const auto matches = candidates
        | std::views::filter([&](const std::string& name) {
              return name != kDefaultService && name.contains(kBrand)
                  && std::ranges::contains(embedded, name);
          })
        | std::ranges::to<std::vector<std::string>>();
    if (matches.empty()) {
        return {.outcome = NoctaliaScan::kLogin, .service = std::string(kDefaultService)};
    }
    if (matches.size() > 1) {
        return {.outcome = NoctaliaScan::kAmbiguous, .service = {}};
    }
    return {.outcome = NoctaliaScan::kService, .service = matches.front()};
}

bool owner_and_mode_trusted(const struct stat& status, uid_t owner) {
    return status.st_uid == owner && (status.st_mode & kUntrustedModeBits) == 0;
}

// Opens the first candidate that exists. A candidate that exists but is not a
// trustworthy regular file ends the search: falling through to a later one
// would let an attacker choose which helper is believed.
NoctaliaDetection inspect_candidates(
    const std::filesystem::path& root, const Candidates& candidates, const ServicePolicy& policy) {
    for (const auto& path : candidates.helpers) {
        const auto opened = open_regular_nofollow(path);
        if (!opened && opened.error() == OpenFailure::kMissing) {
            continue;
        }
        if (!opened) {
            return {.install = NoctaliaInstall::kUnreadable, .helper = path};
        }
        if ((opened->metadata().st_mode & S_IXUSR) == 0) {
            continue;
        }
        if (candidates.require_trusted_owner
            && !owner_and_mode_trusted(opened->metadata(), candidates.trusted_owner)) {
            return {.install = NoctaliaInstall::kUntrusted, .helper = path};
        }
        const auto embedded = embedded_names_cached(path, *opened);
        if (!embedded) {
            return {.install = NoctaliaInstall::kUnreadable, .helper = path};
        }
        return {
            .install = NoctaliaInstall::kHelperFound,
            .helper = path,
            .scan = select_service(*embedded, installed_service_names(root, policy)),
        };
    }
    return {.install = candidates.shell_found ? NoctaliaInstall::kNoHelper
                                              : NoctaliaInstall::kAbsent};
}

} // namespace

NoctaliaScanResult scan_compiled_pam_service(
    std::string_view binary, std::span<const std::string> candidates) {
    return select_service(embedded_branded_names(binary), candidates);
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
    const std::filesystem::path& root, const HelperLookup& lookup, const ServicePolicy& policy) {
    const auto candidates = std::visit(
        [&](const auto& mode) {
            if constexpr (std::is_same_v<std::decay_t<decltype(mode)>, RootHelperLookup>) {
                return root_candidates(root, mode);
            } else {
                return environment_candidates(root, mode);
            }
        },
        lookup);
    return inspect_candidates(root, candidates, policy);
}

NoctaliaCacheStats noctalia_cache_stats() {
    auto& state = cache();
    const auto guard = std::scoped_lock{state.lock};
    return state.stats;
}

} // namespace su::deploy
