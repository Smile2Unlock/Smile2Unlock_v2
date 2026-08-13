// Plain (non-module) TU: Windows API headers are safe to include here.
// Kept outside of the su.app.user module because winnt.h wraps
// <x86intrin.h> in `extern "C"`, which conflicts with the declarations
// imported via `import std;` when compiled as a module unit (GCC 16 mingw).
#include <windows.h>
#include <sddl.h>

#include <cstdint>
#include <memory>
#include <vector>

extern "C" {

int su_win_username_for_uid(char* out, unsigned long cap) {
    DWORD name_length = static_cast<DWORD>(cap);
    if (::GetUserNameA(out, &name_length) == 0 || name_length == 0 || out[0] == '\0') {
        return -1;
    }
    return 0;
}

unsigned int su_win_current_uid() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return 0;
    }
    const auto close_token =
        std::unique_ptr<void, decltype(&::CloseHandle)>(token, &::CloseHandle);

    DWORD length = 0;
    (void)::GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    if (length == 0) {
        return 0;
    }
    auto buffer = std::vector<std::byte>(length);
    if (!::GetTokenInformation(token, TokenUser, buffer.data(), length, &length)) {
        return 0;
    }

    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    const auto sid = user->User.Sid;
    if (sid == nullptr) {
        return 0;
    }
    const auto count = ::GetSidSubAuthorityCount(sid);
    if (count == nullptr || *count == 0) {
        return 0;
    }
    const auto* rid = ::GetSidSubAuthority(sid, *count - 1);
    if (rid == nullptr) {
        return 0;
    }
    return static_cast<unsigned int>(*rid);
}

}  // extern "C"
