#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <sys/types.h>

namespace su::deploy {

// Noctalia compiles its lock-screen PAM service into `noctalia-pam-helper`
// (meson -Dpam_service, default "login"); there is no runtime setting. These
// helpers recover that name so Smile2Unlock can extend the package-owned
// /etc/pam.d/<service> without ever touching /etc/pam.d/login.

enum class NoctaliaScan {
    kLogin,     // No qualifying service name is embedded: the default build.
    kService,   // Exactly one qualifying service name is embedded.
    kAmbiguous, // Several qualifying names match; refuse to guess.
};

struct NoctaliaScanResult {
    NoctaliaScan outcome = NoctaliaScan::kLogin;
    std::string service;
};

enum class NoctaliaInstall {
    kAbsent,       // Neither the shell nor the helper was found.
    kNoHelper,     // `noctalia` exists but predates the PAM helper split.
    kHelperFound,  // The helper binary was located and scanned.
    kUnreadable,   // The helper exists but could not be read safely.
    kUntrusted,    // Root lookup only: wrong owner or group/world-writable.
};

struct NoctaliaDetection {
    NoctaliaInstall install = NoctaliaInstall::kAbsent;
    std::filesystem::path helper;
    NoctaliaScanResult scan;
};

// Unprivileged lookup (the GUI's own inspection). The caller supplies the
// environment explicitly so shared code never reads it behind anyone's back.
struct NoctaliaSearch {
    std::optional<std::filesystem::path> helper_override; // NOCTALIA_PAM_HELPER_PATH
    std::vector<std::filesystem::path> path_directories;  // $PATH entries
};

// Lookup for code that runs as root (the deploy helper). It has no inputs from
// the environment: only fixed prefixes, and the helper must be owned by
// `trusted_owner` (root in production; tests substitute their own uid).
struct RootHelperLookup {
    uid_t trusted_owner = 0;
};

using HelperLookup = std::variant<RootHelperLookup, NoctaliaSearch>;

// Which services may be extended, and which are never acceptable.
struct ServicePolicy {
    std::vector<std::string> denied;  // Services owned by other deployment targets.
    std::string child_service;        // Our own managed substack; never a parent.
};

enum class ServiceRejection {
    kInvalidName,
    kDenied,
    kNotNoctaliaNamed,
    kChildService,
    kNotThinWrapper,
};

[[nodiscard]] bool is_valid_pam_service_name(std::string_view name);
[[nodiscard]] std::string_view service_rejection_text(ServiceRejection reason);

// Name-only part of the predicate (used where the file is not at hand).
[[nodiscard]] std::expected<void, ServiceRejection> check_noctalia_service_name(
    std::string_view name, const ServicePolicy& policy);

// Full predicate: the name checks plus "the auth phase is a single
// `auth include|substack login` line" (or our own managed child substack).
[[nodiscard]] std::expected<void, ServiceRejection> check_noctalia_service(
    std::string_view name, std::string_view pam_content, const ServicePolicy& policy);

// Pure: look for `\0name\0` (or the name at a buffer edge) for every candidate.
// Only candidates whose name contains "noctalia" can be selected.
[[nodiscard]] NoctaliaScanResult scan_compiled_pam_service(
    std::string_view binary, std::span<const std::string> candidates);

// Reads NOCTALIA_PAM_HELPER_PATH and PATH. Only the unprivileged GUI calls this.
[[nodiscard]] NoctaliaSearch noctalia_search_from_environment();

// Thin I/O wrapper. `root` re-roots /etc/pam.d, /usr/lib/pam.d and the fixed
// /usr prefixes. The lookup variant makes the trust level explicit.
[[nodiscard]] NoctaliaDetection detect_noctalia(
    const std::filesystem::path& root, const HelperLookup& lookup, const ServicePolicy& policy);

struct NoctaliaCacheStats {
    std::size_t hits = 0;
    std::size_t misses = 0;
};
[[nodiscard]] NoctaliaCacheStats noctalia_cache_stats();

} // namespace su::deploy
