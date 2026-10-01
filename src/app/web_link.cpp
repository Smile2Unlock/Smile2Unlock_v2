#include "web_link.h"
#include <algorithm>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
extern char** environ;
#endif

namespace su::app {
bool open_web_link(std::string_view url) {
    // These are project/credit links, never commands or arbitrary URI schemes.
    if (!url.starts_with("https://") || url.size() > 4096
        || std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; })) {
        return false;
    }
#ifdef _WIN32
    const auto length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        url.data(), static_cast<int>(url.size()), nullptr, 0);
    if (length <= 0) return false;
    auto wide = std::wstring(length, L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(),
        static_cast<int>(url.size()), wide.data(), length) != length) return false;
    return reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    auto address = std::string(url);
    char command[] = "xdg-open";
    char* args[]{command, address.data(), nullptr};
    pid_t child = 0;
    if (::posix_spawnp(&child, command, nullptr, nullptr, args, environ) != 0) return false;
    std::thread([child] {
        while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    }).detach();
    return true;
#endif
}
}  // namespace su::app
