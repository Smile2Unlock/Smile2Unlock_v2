#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace su::deploy {

// Noctalia merges every *.toml in its config directory, so Smile2Unlock owns a
// dedicated fragment instead of editing the user's own files. The fragment only
// lets Enter on an empty password field start PAM (and so the face scan);
// Noctalia never submits an empty password otherwise.

inline constexpr std::string_view kNoctaliaFragmentName = "smile2unlock.toml";

struct NoctaliaConfigEnvironment {
    std::optional<std::string> noctalia_config_home; // NOCTALIA_CONFIG_HOME
    std::optional<std::string> xdg_config_home;      // XDG_CONFIG_HOME
    std::optional<std::string> home;                 // HOME
};

[[nodiscard]] NoctaliaConfigEnvironment noctalia_config_environment_from_process();

// Mirrors Noctalia's own lookup order; empty when no usable base is known.
[[nodiscard]] std::optional<std::filesystem::path> noctalia_config_directory(
    const NoctaliaConfigEnvironment& environment);

[[nodiscard]] std::string_view noctalia_fragment_content();

// Idempotent. Refuses to replace a file that is not byte-identical to ours.
[[nodiscard]] std::expected<void, std::string> write_noctalia_fragment(
    const std::filesystem::path& directory);

// Idempotent. Refuses to delete a file that was modified or is not a regular file.
[[nodiscard]] std::expected<void, std::string> remove_noctalia_fragment(
    const std::filesystem::path& directory);

} // namespace su::deploy
