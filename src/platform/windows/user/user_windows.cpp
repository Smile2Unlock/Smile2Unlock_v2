// Plain (non-module) TU: Windows API headers are safe to include here.
// Kept outside of the su.app.user module because winnt.h wraps
// <x86intrin.h> in `extern "C"`, which conflicts with the declarations
// imported via `import std;` when compiled as a module unit (GCC 16 mingw).
#include <windows.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
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

// Returns 1 for a locked current session, 0 for unlocked, and -1 when WTS
// cannot provide a trustworthy state. Kept in this plain translation unit so
// Win32 headers do not enter the C++ module that consumes the result.
int su_win_session_locked() {
    LPWSTR raw = nullptr;
    DWORD bytes = 0;
    if (!::WTSQuerySessionInformationW(
            WTS_CURRENT_SERVER_HANDLE,
            WTS_CURRENT_SESSION,
            WTSSessionInfoEx,
            &raw,
            &bytes)) {
        return -1;
    }
    const auto release = std::unique_ptr<void, decltype(&::WTSFreeMemory)>(
        raw, &::WTSFreeMemory);
    if (bytes < sizeof(WTSINFOEXW)) {
        return -1;
    }
    const auto* info = reinterpret_cast<const WTSINFOEXW*>(raw);
    if (info->Level != 1) {
        return -1;
    }
    const auto flags = info->Data.WTSInfoExLevel1.SessionFlags;
    if (flags == WTS_SESSIONSTATE_LOCK) {
        return 1;
    }
    if (flags == WTS_SESSIONSTATE_UNLOCK) {
        return 0;
    }
    return -1;
}

}  // extern "C"

// Quick probe: find a top-level window whose title contains `needle`.
// Returns 1 if found, 0 otherwise, -1 on failure. Does not enumerate.
extern "C" int su_win_find_window(const char* needle, char* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    wchar_t wide[128] = {};
    ::MultiByteToWideChar(
        CP_UTF8, 0, needle != nullptr ? needle : "", -1, wide, 128);
    const HWND h = ::FindWindowW(nullptr, wide);
    if (h == nullptr) {
        return 0;
    }
    RECT rc{};
    ::GetWindowRect(h, &rc);
    snprintf(
        out, cap, "found hwnd=0x%llX vis=%d rect=(%ld,%ld,%ld,%ld)",
        reinterpret_cast<unsigned long long>(h),
        ::IsWindowVisible(h) ? 1 : 0,
        static_cast<long>(rc.left), static_cast<long>(rc.top),
        static_cast<long>(rc.right), static_cast<long>(rc.bottom));
    return 1;
}

// Enumerate this process's top-level windows into `out` as UTF-8 text
// "vis=1 rect=(l,t,r,b) title='...' ...". Returns bytes written.
// Pure Win32; safe to call from a worker thread. The callback accumulates
// into a struct whose address is passed via lParam (no heap allocation).
// NOTE: GetWindowTextW is a synchronous SendMessage for same-process
// windows, which hangs forever if the target thread is not pumping.
// SendMessageTimeoutW with SMTO_ABORTIFHUNG bounds each window to 500ms
// so this probe can never deadlock.
extern "C" int su_win_enum_own_windows(char* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    struct State {
        DWORD pid;
        std::string text;
    };
    State state{::GetCurrentProcessId(), {}};
    ::EnumWindows(
        [](HWND hwnd, LPARAM lparam) -> BOOL {
            auto& st = *reinterpret_cast<State*>(lparam);
            DWORD wpid = 0;
            ::GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid != st.pid) {
                return TRUE;
            }
            wchar_t title[128] = {};
            DWORD_PTR result = 0;
            ::SendMessageTimeoutW(
                hwnd,
                WM_GETTEXT,
                128,
                reinterpret_cast<LPARAM>(title),
                SMTO_ABORTIFHUNG,
                500,
                &result);
            if (result == 0) {
                title[0] = L'\0';
            }
            title[127] = L'\0';
            char narrow[256] = {};
            ::WideCharToMultiByte(
                CP_UTF8, 0, title, -1, narrow, sizeof(narrow), nullptr, nullptr);
            RECT rc{};
            ::GetWindowRect(hwnd, &rc);
            char line[384] = {};
            snprintf(
                line, sizeof(line),
                "vis=%d rect=(%ld,%ld,%ld,%ld) title='%s'",
                ::IsWindowVisible(hwnd) ? 1 : 0,
                static_cast<long>(rc.left), static_cast<long>(rc.top),
                static_cast<long>(rc.right), static_cast<long>(rc.bottom),
                narrow);
            if (!st.text.empty()) {
                st.text += ' ';
            }
            st.text += line;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&state));
    const size_t n = state.text.size();
    if (n >= cap) {
        std::memcpy(out, state.text.data(), cap - 1);
        out[cap - 1] = '\0';
        return static_cast<int>(cap - 1);
    }
    if (n > 0) {
        std::memcpy(out, state.text.data(), n);
    }
    out[n] = '\0';
    return static_cast<int>(n);
}

// Number of OS threads owned by the current process (Toolhelp32), used to
// detect whether background threads are actually alive. Returns -1 on error.
extern "C" int su_win_count_own_threads(void) {
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return -1;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD pid = ::GetCurrentProcessId();
    int count = 0;
    if (::Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == pid) {
                ++count;
            }
        } while (::Thread32Next(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return count;
}
