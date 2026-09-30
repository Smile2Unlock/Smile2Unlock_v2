#ifndef SU_PLATFORM_WINDOWS_USER_USERNAME_UTF8_H_
#define SU_PLATFORM_WINDOWS_USER_USERNAME_UTF8_H_

#include <windows.h>

#include <limits>
#include <string_view>

namespace su::windows {

// The app and Rust/Slint interfaces consume UTF-8, never the system ANSI page.
inline int copy_username_utf8(std::wstring_view username, char* out, unsigned long cap) {
    if (out == nullptr || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    if (cap < 2 || cap > static_cast<unsigned long>(std::numeric_limits<int>::max())
        || username.empty() || username.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return -1;
    }
    const auto written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, username.data(), static_cast<int>(username.size()),
        out, static_cast<int>(cap - 1), nullptr, nullptr);
    if (written <= 0) {
        out[0] = '\0';
        return -1;
    }
    out[written] = '\0';
    return 0;
}

}  // namespace su::windows

#endif  // SU_PLATFORM_WINDOWS_USER_USERNAME_UTF8_H_
