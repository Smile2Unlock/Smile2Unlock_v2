module;

#if defined(_WIN32)
// Windows API implementations live in user_windows.cpp (plain TU); winnt.h
// wraps <x86intrin.h> in `extern "C"`, which conflicts with the declarations
// imported via `import std;` when compiled as a module unit.
extern "C" {
int su_win_username_for_uid(char* out, unsigned long cap);
unsigned int su_win_current_uid();
}
#else
#include <pwd.h>
#include <cerrno>
#include <unistd.h>
#endif

export module su.app.user;

import std;

export namespace su::app {

std::optional<std::string> username_for_uid(std::uint32_t uid);
std::uint32_t current_uid();
std::string current_username(std::string_view fallback);

} // namespace su::app

namespace su::app {

#if defined(_WIN32)

std::optional<std::string> username_for_uid(std::uint32_t) {
    char name[256] = {};
    if (su_win_username_for_uid(name, sizeof(name)) != 0 || name[0] == '\0') {
        return std::nullopt;
    }
    return std::string(name);
}

std::uint32_t current_uid() {
    return static_cast<std::uint32_t>(su_win_current_uid());
}

#else

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

std::uint32_t current_uid() {
    return static_cast<std::uint32_t>(::getuid());
}

#endif

std::string current_username(std::string_view fallback) {
    if (const auto username = username_for_uid(current_uid())) {
        return *username;
    }
    return std::string(fallback);
}

} // namespace su::app
