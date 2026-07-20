module;

#include <cerrno>
#include <sys/stat.h>

export module su.auth.user;

import std;
import su.core.types;

export namespace su::auth {

struct UserPaths {
    std::uint32_t uid = 0;
    std::filesystem::path config;
    std::filesystem::path profiles;
};

UserPaths paths_for_identity(std::uint32_t uid, const std::filesystem::path& home);

bool peer_request_allowed(
    std::uint32_t peer_uid,
    su::app::ControlMessageType message_type,
    std::optional<std::uint32_t> target_uid);

bool secure_user_file(const std::filesystem::path& path, std::uint32_t uid, bool required);

} // namespace su::auth

namespace su::auth {

UserPaths paths_for_identity(std::uint32_t uid, const std::filesystem::path& home) {
    return UserPaths{
        .uid = uid,
        .config = home / ".config" / "smile2unlock" / "config.toml",
        .profiles = home / ".local" / "share" / "smile2unlock" / "profiles.json",
    };
}

bool peer_request_allowed(
    std::uint32_t peer_uid,
    su::app::ControlMessageType message_type,
    std::optional<std::uint32_t> target_uid) {
    if (peer_uid == 0) {
        return true;
    }
    return message_type == su::app::ControlMessageType::kAuthenticate
        && target_uid.has_value()
        && *target_uid == peer_uid;
}

bool secure_user_file(const std::filesystem::path& path, std::uint32_t uid, bool required) {
    struct stat metadata {};
    if (::lstat(path.c_str(), &metadata) != 0) {
        return !required && errno == ENOENT;
    }
    return S_ISREG(metadata.st_mode)
        && static_cast<std::uint32_t>(metadata.st_uid) == uid
        && (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

} // namespace su::auth
