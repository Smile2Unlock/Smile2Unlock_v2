module;

#include <fcntl.h>
#include <linux/openat2.h>
#include <pwd.h>
#include <cerrno>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

export module su.auth.user;

import std;
import su.core.types;

export namespace su::auth {

struct UserPaths {
    std::uint32_t uid = 0;
    std::filesystem::path home;
    std::filesystem::path config;
    std::filesystem::path legacy_profiles;
    std::filesystem::path profiles;
};

enum class UserLookupError {
    kUnknown,
    kNssUnavailable,
    kInvalidHome,
};

enum class UserFileKind {
    kConfig,
    kProfiles,
};

class PinnedUserFile {
public:
    PinnedUserFile() = delete;
    ~PinnedUserFile();
    PinnedUserFile(const PinnedUserFile&) = delete;
    PinnedUserFile& operator=(const PinnedUserFile&) = delete;
    PinnedUserFile(PinnedUserFile&& other) noexcept;
    PinnedUserFile& operator=(PinnedUserFile&& other) noexcept;

    [[nodiscard]] const std::string& proc_path() const { return proc_path_; }

private:
    friend std::expected<std::optional<PinnedUserFile>, std::string> open_user_file(
        const UserPaths&, UserFileKind, bool);
    friend std::expected<void, std::string> remove_pinned_user_file(
        const UserPaths&, UserFileKind, const PinnedUserFile&);
    explicit PinnedUserFile(int fd);

    int fd_ = -1;
    std::uint64_t device_ = 0;
    std::uint64_t inode_ = 0;
    std::string proc_path_;
};

UserPaths paths_for_identity(std::uint32_t uid, const std::filesystem::path& home);
std::expected<UserPaths, UserLookupError> paths_for_username(std::string_view username);
std::string_view user_lookup_error_message(UserLookupError error);

std::expected<std::optional<PinnedUserFile>, std::string> open_user_file(
    const UserPaths& paths,
    UserFileKind kind,
    bool required);
std::expected<void, std::string> remove_pinned_user_file(
    const UserPaths& paths,
    UserFileKind kind,
    const PinnedUserFile& pinned);

bool peer_request_allowed(
    std::uint32_t peer_uid,
    su::app::ControlMessageType message_type,
    std::optional<std::uint32_t> target_uid);

bool authentication_rate_limited(
    std::optional<std::chrono::steady_clock::time_point> last_started,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds minimum_interval);

} // namespace su::auth

namespace su::auth {

UserPaths paths_for_identity(std::uint32_t uid, const std::filesystem::path& home) {
    return UserPaths{
        .uid = uid,
        .home = home,
        .config = home / ".config" / "smile2unlock" / "config.toml",
        .legacy_profiles = home / ".local" / "share" / "smile2unlock" / "profiles.json",
        .profiles = std::filesystem::path{"/var/lib/smile2unlock/users"}
            / std::to_string(uid) / "profiles.s2u",
    };
}

std::expected<UserPaths, UserLookupError> paths_for_username(std::string_view username) {
    auto buffer_size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (buffer_size < 1024) {
        buffer_size = 16 * 1024;
    }

    const auto owned_username = std::string(username);
    auto buffer = std::vector<char>(static_cast<std::size_t>(buffer_size));
    while (buffer.size() <= 1024 * 1024) {
        auto entry = passwd{};
        auto* result = static_cast<passwd*>(nullptr);
        const auto status = ::getpwnam_r(
            owned_username.c_str(),
            &entry,
            buffer.data(),
            buffer.size(),
            &result);
        if (status == 0 && result == nullptr) {
            return std::unexpected(UserLookupError::kUnknown);
        }
        if (status == ERANGE) {
            buffer.resize(buffer.size() * 2);
            continue;
        }
        if (status != 0 || result == nullptr) {
            return std::unexpected(UserLookupError::kNssUnavailable);
        }
        if (entry.pw_dir == nullptr
            || entry.pw_dir[0] == '\0'
            || entry.pw_dir[0] != '/') {
            return std::unexpected(UserLookupError::kInvalidHome);
        }
        return paths_for_identity(
            static_cast<std::uint32_t>(entry.pw_uid),
            std::filesystem::path(entry.pw_dir));
    }
    return std::unexpected(UserLookupError::kNssUnavailable);
}

std::string_view user_lookup_error_message(UserLookupError error) {
    switch (error) {
    case UserLookupError::kUnknown: return "unknown PAM user";
    case UserLookupError::kNssUnavailable: return "NSS user lookup unavailable";
    case UserLookupError::kInvalidHome: return "user home is unavailable or unsupported";
    }
    std::unreachable();
}

bool peer_request_allowed(
    std::uint32_t peer_uid,
    su::app::ControlMessageType message_type,
    std::optional<std::uint32_t> target_uid) {
    if (peer_uid == 0) {
        return true;
    }
    const auto account_operation = message_type == su::app::ControlMessageType::kAuthenticate
        || message_type == su::app::ControlMessageType::kListProfiles
        || message_type == su::app::ControlMessageType::kEnrollProfile
        || message_type == su::app::ControlMessageType::kDeleteProfile
        || message_type == su::app::ControlMessageType::kMigrateProfiles
        || message_type == su::app::ControlMessageType::kVerifyProfile;
    return account_operation
        && target_uid.has_value()
        && *target_uid == peer_uid;
}

namespace {

constexpr auto kMaxConfigBytes = std::uintmax_t{1024 * 1024};
constexpr auto kMaxProfileBytes = std::uintmax_t{16 * 1024 * 1024};

int open_beneath(int directory, const char* path, int flags, bool beneath) {
    const auto how = open_how{
        .flags = static_cast<std::uint64_t>(flags),
        .mode = 0,
        .resolve = static_cast<std::uint64_t>(
            (beneath ? RESOLVE_BENEATH : 0)
            | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS),
    };
    const auto opened = static_cast<int>(::syscall(SYS_openat2, directory, path, &how, sizeof(how)));
    if (opened >= 0 || errno != ENOSYS) {
        return opened;
    }

    // Some service sandboxes filter openat2 even when the host kernel supports
    // it. Keep the same no-symlink invariant with descriptor-relative openat.
    const auto components = std::string(path);
    auto start = std::size_t{0};
    auto current = beneath
        ? ::dup(directory)
        : ::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (current < 0) {
        return -1;
    }
    if (!beneath) {
        if (components.empty() || components.front() != '/') {
            (void)::close(current);
            errno = EINVAL;
            return -1;
        }
        start = 1;
        if (start == components.size()) {
            return current;
        }
    }
    while (start < components.size()) {
        const auto separator = components.find('/', start);
        const auto length = separator == std::string::npos
            ? components.size() - start
            : separator - start;
        if (length == 0 || components.compare(start, length, ".") == 0
            || components.compare(start, length, "..") == 0) {
            (void)::close(current);
            errno = EINVAL;
            return -1;
        }
        const auto final_component = separator == std::string::npos;
        const auto component = components.substr(start, length);
        const auto next = ::openat(
            current,
            component.c_str(),
            (final_component ? flags : O_PATH | O_DIRECTORY)
                | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            const auto saved_errno = errno;
            (void)::close(current);
            errno = saved_errno;
            return -1;
        }
        (void)::close(current);
        if (final_component) {
            return next;
        }
        current = next;
        start = separator + 1;
    }
    (void)::close(current);
    errno = EINVAL;
    return -1;
}

std::filesystem::path relative_path(UserFileKind kind) {
    return kind == UserFileKind::kConfig
        ? std::filesystem::path{".config/smile2unlock/config.toml"}
        : std::filesystem::path{".local/share/smile2unlock/profiles.json"};
}

std::uintmax_t max_file_size(UserFileKind kind) {
    return kind == UserFileKind::kConfig ? kMaxConfigBytes : kMaxProfileBytes;
}

} // namespace

PinnedUserFile::PinnedUserFile(int fd)
    : fd_(fd), proc_path_(std::format("/proc/self/fd/{}", fd)) {
    struct stat metadata {};
    if (::fstat(fd_, &metadata) == 0) {
        device_ = static_cast<std::uint64_t>(metadata.st_dev);
        inode_ = static_cast<std::uint64_t>(metadata.st_ino);
    }
}

PinnedUserFile::~PinnedUserFile() {
    if (fd_ >= 0) {
        (void)::close(fd_);
    }
}

PinnedUserFile::PinnedUserFile(PinnedUserFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      device_(std::exchange(other.device_, 0)),
      inode_(std::exchange(other.inode_, 0)),
      proc_path_(std::move(other.proc_path_)) {}

PinnedUserFile& PinnedUserFile::operator=(PinnedUserFile&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (fd_ >= 0) {
        (void)::close(fd_);
    }
    fd_ = std::exchange(other.fd_, -1);
    device_ = std::exchange(other.device_, 0);
    inode_ = std::exchange(other.inode_, 0);
    proc_path_ = std::move(other.proc_path_);
    return *this;
}

std::expected<std::optional<PinnedUserFile>, std::string> open_user_file(
    const UserPaths& paths,
    UserFileKind kind,
    bool required) {
    if (!paths.home.is_absolute()) {
        return std::unexpected("user home must be an absolute path");
    }

    const auto home_fd = open_beneath(
        AT_FDCWD,
        paths.home.c_str(),
        O_PATH | O_DIRECTORY | O_CLOEXEC,
        false);
    if (home_fd < 0) {
        if (!required && errno == ENOENT) {
            return std::optional<PinnedUserFile>{};
        }
        return std::unexpected(std::format("unable to open user home safely (errno={})", errno));
    }

    const auto file_fd = open_beneath(
        home_fd,
        relative_path(kind).c_str(),
        O_RDONLY | O_CLOEXEC,
        true);
    const auto saved_errno = errno;
    (void)::close(home_fd);
    if (file_fd < 0) {
        if (!required && saved_errno == ENOENT) {
            return std::optional<PinnedUserFile>{};
        }
        return std::unexpected(std::format(
            "unsafe, inaccessible, or missing user data (errno={})", saved_errno));
    }

    struct stat metadata {};
    if (::fstat(file_fd, &metadata) != 0
        || !S_ISREG(metadata.st_mode)
        || (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0
        || static_cast<std::uintmax_t>(metadata.st_size) > max_file_size(kind)) {
        (void)::close(file_fd);
        return std::unexpected("unsafe, inaccessible, or oversized user data");
    }
    if (static_cast<std::uint32_t>(metadata.st_uid) != paths.uid) {
        (void)::close(file_fd);
        return std::unexpected("user data has the wrong owner");
    }
    auto pinned = PinnedUserFile{file_fd};
    return std::optional<PinnedUserFile>{std::move(pinned)};
}

std::expected<void, std::string> remove_pinned_user_file(
    const UserPaths& paths,
    UserFileKind kind,
    const PinnedUserFile& pinned) {
    if (kind != UserFileKind::kProfiles || pinned.fd_ < 0
        || pinned.device_ == 0 || pinned.inode_ == 0) {
        return std::unexpected("invalid pinned user file");
    }
    const auto home_fd = open_beneath(
        AT_FDCWD,
        paths.home.c_str(),
        O_PATH | O_DIRECTORY | O_CLOEXEC,
        false);
    if (home_fd < 0) {
        return std::unexpected("unable to reopen user home safely");
    }
    const auto directory_fd = open_beneath(
        home_fd,
        ".local/share/smile2unlock",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        true);
    const auto directory_error = errno;
    (void)::close(home_fd);
    if (directory_fd < 0) {
        return std::unexpected(std::format(
            "unable to reopen user data directory safely (errno={})", directory_error));
    }

    struct stat current {};
    const auto matches = ::fstatat(
        directory_fd, "profiles.json", &current, AT_SYMLINK_NOFOLLOW) == 0
        && S_ISREG(current.st_mode)
        && static_cast<std::uint64_t>(current.st_dev) == pinned.device_
        && static_cast<std::uint64_t>(current.st_ino) == pinned.inode_;
    if (!matches) {
        (void)::close(directory_fd);
        return std::unexpected("legacy profile changed during migration");
    }
    if (::unlinkat(directory_fd, "profiles.json", 0) != 0 || ::fsync(directory_fd) != 0) {
        const auto saved_errno = errno;
        (void)::close(directory_fd);
        return std::unexpected(std::format(
            "failed to remove migrated profile (errno={})", saved_errno));
    }
    (void)::close(directory_fd);
    return {};
}

bool authentication_rate_limited(
    std::optional<std::chrono::steady_clock::time_point> last_started,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds minimum_interval) {
    if (!last_started) {
        return false;
    }
    return now < *last_started
        || now - *last_started < minimum_interval;
}

} // namespace su::auth
