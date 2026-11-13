#include "platform/linux/deploy_client/noctalia_config.h"

#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

#include <unistd.h>

namespace su::deploy {
namespace {

constexpr std::string_view kFragmentContent =
    "# Managed by Smile2Unlock. Turn the Noctalia lock screen off in the app to remove it.\n"
    "# Lets Enter on an empty password field start PAM, which runs the face scan.\n"
    "# If the scan fails the password stack still rejects the empty password.\n"
    "[lockscreen]\n"
    "allow_empty_password = true\n";

std::optional<std::string> environment_value(const char* name) {
    const auto* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::expected<std::optional<std::string>, std::string> read_existing(
    const std::filesystem::path& path) {
    auto error = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return std::nullopt;
    }
    if (error || status.type() != std::filesystem::file_type::regular) {
        return std::unexpected(std::format("{} is not a regular file; leaving it alone",
                                           path.string()));
    }
    auto input = std::ifstream(path, std::ios::binary);
    if (!input) {
        return std::unexpected(std::format("cannot read {}", path.string()));
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

NoctaliaConfigEnvironment noctalia_config_environment_from_process() {
    return {
        .noctalia_config_home = environment_value("NOCTALIA_CONFIG_HOME"),
        .xdg_config_home = environment_value("XDG_CONFIG_HOME"),
        .home = environment_value("HOME"),
    };
}

std::optional<std::filesystem::path> noctalia_config_directory(
    const NoctaliaConfigEnvironment& environment) {
    if (environment.noctalia_config_home) {
        return std::filesystem::path(*environment.noctalia_config_home);
    }
    if (environment.xdg_config_home) {
        return std::filesystem::path(*environment.xdg_config_home) / "noctalia";
    }
    if (environment.home) {
        return std::filesystem::path(*environment.home) / ".config" / "noctalia";
    }
    return std::nullopt;
}

std::string_view noctalia_fragment_content() {
    return kFragmentContent;
}

std::expected<void, std::string> write_noctalia_fragment(const std::filesystem::path& directory) {
    const auto path = directory / kNoctaliaFragmentName;
    const auto existing = read_existing(path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (*existing) {
        if (**existing == kFragmentContent) {
            return {};
        }
        return std::unexpected(std::format(
            "{} was modified; refusing to overwrite it", path.string()));
    }
    auto error = std::error_code{};
    std::filesystem::create_directories(directory, error);
    if (error) {
        return std::unexpected(std::format("cannot create {}", directory.string()));
    }
    // Rename keeps Noctalia's file watcher from seeing a half-written fragment.
    const auto temporary = directory / std::format(".{}.{}.tmp", kNoctaliaFragmentName, ::getpid());
    {
        auto output = std::ofstream(temporary, std::ios::binary | std::ios::trunc);
        output << kFragmentContent;
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary, error);
            return std::unexpected(std::format("cannot write {}", temporary.string()));
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return std::unexpected(std::format("cannot install {}", path.string()));
    }
    return {};
}

std::expected<void, std::string> remove_noctalia_fragment(const std::filesystem::path& directory) {
    const auto path = directory / kNoctaliaFragmentName;
    const auto existing = read_existing(path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (!*existing) {
        return {};
    }
    if (**existing != kFragmentContent) {
        return std::unexpected(std::format(
            "{} was modified; refusing to delete it", path.string()));
    }
    auto error = std::error_code{};
    std::filesystem::remove(path, error);
    if (error) {
        return std::unexpected(std::format("cannot remove {}", path.string()));
    }
    return {};
}

} // namespace su::deploy
