#include "platform/linux/deploy_client/noctalia_config.h"

#include "platform/linux/deploy/safe_read.h"

#include <cerrno>
#include <cstdlib>
#include <format>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace su::deploy {
namespace {

// Anything longer than our own fragment cannot be it.
constexpr std::size_t kMaxFragmentBytes = 4096;
constexpr mode_t kFragmentMode = 0644;

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

// Reads through one O_NOFOLLOW descriptor so the file that is inspected is the
// file that is compared; a symlink swapped in afterwards cannot redirect us.
std::expected<std::optional<std::string>, std::string> read_existing(
    const std::filesystem::path& path) {
    const auto opened = open_regular_nofollow(path);
    if (!opened) {
        switch (opened.error()) {
        case OpenFailure::kMissing:
            return std::nullopt;
        case OpenFailure::kUnreadable:
            return std::unexpected(std::format("cannot read {}", path.string()));
        case OpenFailure::kSymlink:
        case OpenFailure::kNotRegular:
            return std::unexpected(std::format("{} is not a regular file; leaving it alone",
                                               path.string()));
        }
        std::unreachable();
    }
    auto content = read_capped(*opened, kMaxFragmentBytes);
    if (!content) {
        return std::unexpected(std::format("{} is not the file we wrote; leaving it alone",
                                           path.string()));
    }
    return std::move(*content);
}

// O_EXCL|O_NOFOLLOW: never write through a pre-planted file or symlink.
std::expected<void, std::string> write_exclusive(
    const std::filesystem::path& path, std::string_view content) {
    const int fd = ::open(path.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, kFragmentMode);
    if (fd < 0) {
        return std::unexpected(std::format("cannot create {}", path.string()));
    }
    auto offset = std::size_t{0};
    while (offset < content.size()) {
        const auto written = ::write(fd, content.data() + offset, content.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            ::close(fd);
            ::unlink(path.c_str()); // We created it (O_EXCL), so it is ours to clean up.
            return std::unexpected(std::format("cannot write {}", path.string()));
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::close(fd) != 0) {
        ::unlink(path.c_str());
        return std::unexpected(std::format("cannot write {}", path.string()));
    }
    return {};
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
    // Not removed on failure: if O_EXCL failed, the name belongs to someone else.
    if (const auto written = write_exclusive(temporary, kFragmentContent); !written) {
        return written;
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
