module;

#include <pwd.h>
#include <cerrno>
#include <unistd.h>

export module su.app.user;

import std;

export namespace su::app {

std::optional<std::string> username_for_uid(std::uint32_t uid);
std::uint32_t current_uid();
std::string current_username(std::string_view fallback);

} // namespace su::app

namespace su::app {

std::optional<std::string> username_for_uid(std::uint32_t uid) {
    auto buffer_size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (buffer_size < 1024) {
        buffer_size = 16 * 1024;
    }
    auto buffer = std::vector<char>(static_cast<std::size_t>(buffer_size));
    while (buffer.size() <= 1024 * 1024) {
        auto entry = passwd{};
        auto* result = static_cast<passwd*>(nullptr);
        const auto status = ::getpwuid_r(
            static_cast<uid_t>(uid),
            &entry,
            buffer.data(),
            buffer.size(),
            &result);
        if (status == 0 && result != nullptr && entry.pw_name != nullptr
            && entry.pw_name[0] != '\0') {
            return std::string(entry.pw_name);
        }
        if (status != ERANGE) {
            return std::nullopt;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::nullopt;
}

std::string current_username(std::string_view fallback) {
    if (const auto username = username_for_uid(current_uid())) {
        return *username;
    }
    return std::string(fallback);
}

std::uint32_t current_uid() {
    return static_cast<std::uint32_t>(::getuid());
}

} // namespace su::app
