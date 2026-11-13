#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace su::deploy {

// Noctalia compiles its lock-screen PAM service into `noctalia-pam-helper`
// (meson -Dpam_service, default "login"); there is no runtime setting. These
// helpers recover that name so Smile2Unlock can extend the package-owned
// /etc/pam.d/<service> without ever touching /etc/pam.d/login.

enum class NoctaliaScan {
    kLogin,     // No other service name is embedded: the default build.
    kService,   // Exactly one plausible service name is embedded.
    kAmbiguous, // Several unrelated names match; refuse to guess.
};

struct NoctaliaScanResult {
    NoctaliaScan outcome = NoctaliaScan::kLogin;
    std::string service;
};

enum class NoctaliaInstall {
    kAbsent,       // Neither the shell nor the helper was found.
    kNoHelper,     // `noctalia` exists but predates the PAM helper split.
    kHelperFound,  // The helper binary was located and scanned.
    kUnreadable,   // The helper exists but could not be read.
};

struct NoctaliaDetection {
    NoctaliaInstall install = NoctaliaInstall::kAbsent;
    std::filesystem::path helper;
    NoctaliaScanResult scan;
};

struct NoctaliaSearch {
    std::optional<std::filesystem::path> helper_override; // NOCTALIA_PAM_HELPER_PATH
    std::vector<std::filesystem::path> path_directories;  // $PATH entries
};

[[nodiscard]] bool is_valid_pam_service_name(std::string_view name);

// Pure: look for `\0name\0` (or the name at a buffer edge) for every candidate.
[[nodiscard]] NoctaliaScanResult scan_compiled_pam_service(
    std::string_view binary, std::span<const std::string> candidates);

// Reads NOCTALIA_PAM_HELPER_PATH and PATH from the process environment.
[[nodiscard]] NoctaliaSearch noctalia_search_from_environment();

// Thin I/O wrapper. `root` re-roots /etc/pam.d and the fixed /usr prefixes.
[[nodiscard]] NoctaliaDetection detect_noctalia(
    const std::filesystem::path& root, const NoctaliaSearch& search);

} // namespace su::deploy
