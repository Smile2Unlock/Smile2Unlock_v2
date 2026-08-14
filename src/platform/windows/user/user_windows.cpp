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

// Write the recognition trigger policy to HKLM\SOFTWARE\Smile2Unlock\Recognition,
// the key the credential provider reads at lock-screen time. Requires admin
// rights (the GUI runs elevated). Failures are reported as non-zero so the
// caller can surface a diagnostic; the config.toml copy remains authoritative
// for the GUI itself.
int su_win_write_recognition_registry(
    unsigned int mode,
    unsigned int auto_delay_sec,
    unsigned int retry_delay_sec,
    unsigned int timeout_sec) {
    const wchar_t* const key_path = L"SOFTWARE\\Smile2Unlock\\Recognition";
    HKEY key = nullptr;
    if (::RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            key_path,
            0,
            nullptr,
            0,
            KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS) {
        return -1;
    }
    struct RegKeyCloser {
        void operator()(HKEY handle) const { ::RegCloseKey(handle); }
    };
    const auto close_key =
        std::unique_ptr<std::remove_pointer_t<HKEY>, RegKeyCloser>(key);
    const auto set_dword = [key](const wchar_t* name, unsigned int value) {
        return ::RegSetValueExW(
                   key,
                   name,
                   0,
                   REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value),
                   sizeof(value))
            == ERROR_SUCCESS;
    };
    int result = 0;
    if (!set_dword(L"RecognitionMode", mode)
        || !set_dword(L"AutoDelaySec", auto_delay_sec)
        || !set_dword(L"RetryDelaySec", retry_delay_sec)
        || !set_dword(L"TimeoutSec", timeout_sec)) {
        result = -2;
    }
    return result;
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
