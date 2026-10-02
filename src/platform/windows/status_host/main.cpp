// Small, non-interactive window on LogonUI's desktop. No authentication or
// input handling lives here; stopping this process only removes the hint.
#include <windows.h>
#include <wtsapi32.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <optional>
#include <string>
#include "protocol.h"

namespace {
using su::status::Phase;
using su::status::Snapshot;
constexpr wchar_t window_class[] = L"Smile2UnlockRecognitionStatus";

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

bool has_ui_access() {
    Handle token;
    DWORD enabled = 0, bytes = 0;
    return OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)
        && GetTokenInformation(token.value, TokenUIAccess, &enabled, sizeof(enabled), &bytes)
        && enabled != 0;
}

// USER32 checks UIAccess when the process initializes. Setting it after a
// window has been created does not enable CreateWindowInBand. Launch a fresh
// copy with the flag already present on its primary token instead.
std::optional<DWORD> run_ui_access_display(HANDLE parent) {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE
            | TOKEN_ADJUST_PRIVILEGES, &token.value)) return std::nullopt;
    alignas(TOKEN_USER) std::array<BYTE, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> user{};
    DWORD bytes = 0;
    if (!GetTokenInformation(token.value, TokenUser, user.data(), user.size(), &bytes)
        || !IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, WinLocalSystemSid)) {
        return std::nullopt;
    }
    TOKEN_PRIVILEGES privilege{}, previous{};
    privilege.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, L"SeTcbPrivilege", &privilege.Privileges[0].Luid)) return std::nullopt;
    privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token.value, FALSE, &privilege, sizeof(previous), &previous, &bytes)
        || GetLastError() != ERROR_SUCCESS) return std::nullopt;
    Handle child_token;
    DWORD enabled = 1;
    const bool prepared = DuplicateTokenEx(token.value, TOKEN_QUERY | TOKEN_DUPLICATE
            | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, nullptr, SecurityImpersonation,
            TokenPrimary, &child_token.value)
        && SetTokenInformation(child_token.value, TokenUIAccess, &enabled, sizeof(enabled));
    AdjustTokenPrivileges(token.value, FALSE, &previous, 0, nullptr, nullptr);
    if (!prepared) return std::nullopt;

    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, std::size(executable));
    if (!length || length >= std::size(executable)) return std::nullopt;
    // Preserve the original, already quoted arguments; append only a fixed
    // recursion guard. lpApplicationName pins the executable being launched.
    auto command = std::wstring(GetCommandLineW()) + L" --uiaccess-child";
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) return std::nullopt;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    wchar_t desktop[] = L"WinSta0\\Winlogon";
    startup.lpDesktop = desktop;
    PROCESS_INFORMATION process{};
    if (!CreateProcessAsUserW(child_token.value, executable, command.data(), nullptr, nullptr,
            FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return std::nullopt;
    }
    Handle child{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job.value, child.value)
        || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(child.value, 5);
        WaitForSingleObject(child.value, INFINITE);
        return std::nullopt;
    }
    // Killing the host from the credential provider closes the job and also
    // kills this display copy. No orphan UIAccess process survives UnAdvise.
    const HANDLE waits[] = {child.value, parent};
    WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    DWORD result = 0;
    if (!GetExitCodeProcess(child.value, &result) || result == STILL_ACTIVE) result = 0;
    return result;
}

HWND create_status_window(ATOM window_atom) {
    constexpr DWORD style = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    // Optional OS export: never hard-link a private API or require it for
    // authentication. Older systems retain the ordinary LogonUI overlay.
    using CreateWindowInBand = HWND(WINAPI*)(DWORD, ATOM, LPCWSTR, DWORD,
        int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID, DWORD);
    const auto create_in_band = std::bit_cast<CreateWindowInBand>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "CreateWindowInBand"));
    if (create_in_band && has_ui_access()) {
        constexpr DWORD ui_access_band = 2;
        if (auto window = create_in_band(style, window_atom, L"Smile2Unlock", WS_POPUP,
                0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr, ui_access_band)) {
            return window;
        }
        OutputDebugStringW(L"Smile2Unlock: UIAccess band unavailable; using ordinary status window.\n");
    }
    return CreateWindowExW(style, window_class, L"Smile2Unlock", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}
struct Text {
    std::wstring title;
    std::wstring detail;
};
Text caption(Snapshot state, bool chinese) {
    const auto retry = chinese
        ? (state.automatic ? L"请使用密码，或重新选择人脸登录" : L"按 Enter 重试，或输入密码")
        : (state.automatic ? L"Use your password or reselect face sign-in" : L"Press Enter to retry, or enter your password");
    switch (state.phase) {
    case Phase::waiting: return chinese
        ? Text{L"按 Enter 开始人脸识别", L"也可点击登录箭头，或直接输入密码"}
        : Text{L"Press Enter to recognize your face", L"Or use the sign-in arrow or your password"};
    case Phase::initial_delay:
        if (!state.seconds) return chinese ? Text{L"正在启动人脸识别…", L"请看向摄像头"}
            : Text{L"Starting face recognition…", L"Look toward the camera"};
        return chinese ? Text{std::to_wstring(state.seconds) + L" 秒后开始人脸识别", L"也可直接输入密码"}
            : Text{L"Face recognition starts in " + std::to_wstring(state.seconds) + L"s", L"You can also enter your password"};
    case Phase::recognizing: return chinese ? Text{L"正在识别你…", L"请看向摄像头"}
        : Text{L"Looking for you…", L"Look toward the camera"};
    case Phase::retry_delay: return chinese ? Text{L"暂未通过验证", std::to_wstring(state.seconds) + L" 秒后重试，也可使用密码"}
        : Text{L"Face not verified yet", L"Retrying in " + std::to_wstring(state.seconds) + L"s; you can use your password"};
    case Phase::ready: return chinese ? Text{L"识别通过", L"正在准备登录"}
        : Text{L"Face verified", L"Preparing to sign in"};
    case Phase::submitting: return chinese ? Text{L"正在登录…", L""} : Text{L"Signing in…", L""};
    case Phase::no_match: return {chinese ? L"暂未通过验证" : L"Face not verified", retry};
    case Phase::timed_out: return {chinese ? L"人脸识别已超时" : L"Face recognition timed out", retry};
    case Phase::unavailable: return chinese ? Text{L"人脸识别暂不可用", L"请使用密码登录"}
        : Text{L"Face recognition is unavailable", L"Use your password to sign in"};
    case Phase::login_failed: return chinese ? Text{L"Windows 未接受登录", L"请使用密码，并检查保存的账户凭据"}
        : Text{L"Windows could not sign you in", L"Use your password and check your saved credentials"};
    default: return {};
    }
}

std::wstring desktop_name(HDESK desktop) {
    wchar_t name[128]{};
    DWORD length = 0;
    if (!desktop || !GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &length)) return {};
    return name;
}

bool on_input_desktop() {
    const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return false;
    const auto name = desktop_name(desktop);
    CloseDesktop(desktop);
    const auto current = desktop_name(GetThreadDesktop(GetCurrentThreadId()));
    return !name.empty() && _wcsicmp(name.c_str(), current.c_str()) == 0;
}

bool session_locked(DWORD session) {
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx, &buffer, &bytes)) return false;
    const auto* info = reinterpret_cast<const WTSINFOEXW*>(buffer);
    const auto locked = bytes >= sizeof(WTSINFOEXW) && info->Level == 1
        && info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
    WTSFreeMemory(buffer);
    return locked;
}

struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    std::uint32_t* pixels = nullptr;
    int width = 0, height = 0;
    Surface(int w, int h) : width(w), height(h) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
            reinterpret_cast<void**>(&pixels), nullptr, 0);
        if (dc && bitmap) previous = SelectObject(dc, bitmap);
    }
    ~Surface() {
        if (previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    bool valid() const { return dc && bitmap && pixels; }
};

class Display {
    HWND window_ = nullptr;
    Gdiplus::PrivateFontCollection fonts_;
    std::unique_ptr<Gdiplus::FontFamily> icon_font_;
    Gdiplus::FontFamily text_font_{L"Segoe UI"};
    std::unique_ptr<Surface> surface_, sample_;
    POINT position_{};
    float scale_ = 1;
    bool visible_ = false, dark_ = false, sampled_ = false, chinese_ = false;
    bool candidate_ = false;
    unsigned candidate_count_ = 0;
    ULONGLONG shown_at_ = 0, sampled_at_ = 0, valid_sample_at_ = 0;
    std::uint32_t painted_state_ = ~0U;
    unsigned painted_frame_ = ~0U;
    bool painted_dark_ = false;

    void text(Gdiplus::Graphics& graphics, const std::wstring& value,
              const Gdiplus::FontFamily& font, float size, float top, float height) const {
        if (value.empty()) return;
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::GraphicsPath path;
        path.AddString(value.c_str(), static_cast<INT>(value.size()), &font,
            Gdiplus::FontStyleRegular, size * scale_,
            Gdiplus::RectF(4 * scale_, top * scale_, surface_->width - 8 * scale_, height * scale_), &format);
        const BYTE foreground = dark_ ? 16 : 255;
        const BYTE opposite = dark_ ? 255 : 0;
        Gdiplus::Pen outline(Gdiplus::Color(150, opposite, opposite, opposite), 1.15F * scale_);
        outline.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, foreground, foreground, foreground));
        graphics.DrawPath(&outline, &path);
        graphics.FillPath(&fill, &path);
    }

    void sample_background(ULONGLONG now) {
        if (sampled_ && now - sampled_at_ < 750) return;
        sampled_at_ = now;
        const auto screen = GetDC(nullptr);
        if (!screen) return;
        const auto copied = BitBlt(sample_->dc, 0, 0, surface_->width, surface_->height,
            screen, position_.x, position_.y, SRCCOPY | CAPTUREBLT);
        ReleaseDC(nullptr, screen);
        if (!copied) return;
        GdiFlush();
        // Exclude every pixel occupied by our last displayed glyph/text,
        // including antialiased outlines. The remaining transparent area
        // samples the actual local background, without hiding the window or
        // feeding our own white/black text back into color selection.
        double total = 0;
        unsigned count = 0;
        const auto linear = [](unsigned channel) {
            const double c = channel / 255.0;
            return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        };
        for (int y = 0; y < surface_->height; y += 4) {
            for (int x = 0; x < surface_->width; x += 4) {
                const auto index = y * surface_->width + x;
                if (visible_ && (surface_->pixels[index] >> 24) != 0) continue;
                const auto pixel = sample_->pixels[index];
                total += .2126 * linear((pixel >> 16) & 255) + .7152 * linear((pixel >> 8) & 255) + .0722 * linear(pixel & 255);
                ++count;
            }
        }
        if (count < 16) return;
        valid_sample_at_ = now;
        const auto next = su::status::use_dark_foreground(total / count, dark_, !sampled_);
        if (!sampled_) dark_ = next;
        else if (next == candidate_) {
            if (++candidate_count_ >= 2) dark_ = next;
        } else { candidate_ = next; candidate_count_ = 1; }
        sampled_ = true;
    }

public:
    explicit Display(ATOM window_atom) {
        chinese_ = PRIMARYLANGID(GetSystemDefaultUILanguage()) == LANG_CHINESE;
        wchar_t windows_directory[MAX_PATH]{};
        if (GetWindowsDirectoryW(windows_directory, MAX_PATH)) {
            const auto path = std::wstring(windows_directory) + L"\\SystemResources\\Windows.UI.BioFeedback\\Fonts\\NUIMDL2.ttf";
            if (fonts_.AddFontFile(path.c_str()) == Gdiplus::Ok && fonts_.GetFamilyCount() > 0) {
                auto family = std::make_unique<Gdiplus::FontFamily>();
                INT found = 0;
                if (fonts_.GetFamilies(1, family.get(), &found) == Gdiplus::Ok && found == 1) icon_font_ = std::move(family);
            }
        }
        window_ = create_status_window(window_atom);
    }
    ~Display() { if (window_) DestroyWindow(window_); }
    void hide() {
        if (visible_) ShowWindow(window_, SW_HIDE);
        visible_ = false;
        sampled_ = false;
        candidate_count_ = 0;
    }
    void update(Snapshot state, ULONGLONG now) {
        const auto foreground = GetForegroundWindow();
        // The window must be on the input desktop, including the Default
        // desktop used by the 26H2 clock curtain. The main loop separately
        // requires a locked session before rendering on Default.
        if (!window_ || state.phase == Phase::hidden || !on_input_desktop()) {
            hide(); return;
        }
        const auto monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) { hide(); return; }
        const auto dpi = GetDpiForWindow(foreground);
        const auto scale = (dpi ? dpi : 96) / 96.0F;
        const auto available_width = info.rcMonitor.right - info.rcMonitor.left;
        const auto available_height = info.rcMonitor.bottom - info.rcMonitor.top;
        // Respect small displays and large accessibility scaling; stay above
        // the clock on the initial curtain and clear of the user avatar and
        // password controls after a tile is selected.
        const auto fit = std::min({scale, available_width / 500.0F, available_height / 880.0F});
        const int width = static_cast<int>(440 * fit), height = static_cast<int>(132 * fit);
        const POINT position{info.rcMonitor.left + (available_width - width) / 2,
            info.rcMonitor.top + static_cast<LONG>(available_height * .015)};
        if (!surface_ || surface_->width != width || surface_->height != height ||
            position.x != position_.x || position.y != position_.y) {
            hide();
            surface_ = std::make_unique<Surface>(width, height);
            sample_ = std::make_unique<Surface>(width, height);
            if (!surface_->valid() || !sample_->valid()) { surface_.reset(); return; }
            position_ = position;
            scale_ = fit;
            std::fill_n(surface_->pixels, width * height, 0);
        }
        sample_background(now);
        // When capture fails on a desktop transition, do not reuse pixels
        // from the previous screen or display an arbitrary contrast choice.
        if (!sampled_ || now - valid_sample_at_ > 1500) { hide(); return; }
        const auto frame = state.phase == Phase::recognizing
            ? static_cast<unsigned>((now / 45) % 60) : 0U;
        const auto packed = static_cast<unsigned>(state.phase) | (state.automatic ? 16U : 0U) | (state.seconds << 8);
        if (visible_ && now - shown_at_ >= 180 && painted_state_ == packed
            && painted_frame_ == frame && painted_dark_ == dark_) return;
        Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppPARGB,
            reinterpret_cast<BYTE*>(surface_->pixels));
        Gdiplus::Graphics graphics(&bitmap);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        const auto successful = state.phase == Phase::ready || state.phase == Phase::submitting;
        if (icon_font_) {
            wchar_t glyph = successful ? L'\xe75d' : L'\xe600';
            if (state.phase == Phase::recognizing) {
                glyph = static_cast<wchar_t>(frame < 30 ? 0xe700 + frame : 0xe720 + frame - 30);
            }
            text(graphics, std::wstring(1, glyph), *icon_font_, 52, 0, 62);
        } else {
            // Public system-font fallback; never depend on a private font
            // being present after an OS upgrade.
            const Gdiplus::FontFamily fallback(L"Segoe MDL2 Assets");
            text(graphics, successful ? L"\xe73e" : L"\xeb68", fallback, 43, 0, 62);
        }
        const auto copy = caption(state, chinese_);
        text(graphics, copy.title, text_font_, 17, 66, 28);
        text(graphics, copy.detail, text_font_, 12.5F, 96, 26);
        graphics.Flush(Gdiplus::FlushIntentionSync);
        if (!visible_) shown_at_ = now;
        const auto alpha = static_cast<BYTE>(std::min<ULONGLONG>(255, (now - shown_at_) * 255 / 150));
        // Win32 declares these input-only arguments as mutable pointers.
        BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        SIZE size{width, height};
        POINT source{};
        if (UpdateLayeredWindow(window_, nullptr, &position_, &size, surface_->dc,
                &source, 0, &blend, ULW_ALPHA)) {
            SetWindowPos(window_, HWND_TOPMOST, position_.x, position_.y, width, height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            visible_ = true;
            painted_state_ = packed;
            painted_frame_ = frame;
            painted_dark_ = dark_;
        } else hide();
    }
};

LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool ui_access_child = argc == 4 && std::wcscmp(argv[3], L"--uiaccess-child") == 0;
    if (argc != 3 && !ui_access_child) return 2;
    wchar_t* end = nullptr;
    const auto parent_id = std::wcstoul(argv[2], &end, 10);
    if (!parent_id || *end || parent_id > MAXDWORD) return 2;
    Handle parent{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_id)};
    if (!parent.value) return 3;
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    if (!QueryFullProcessImageNameW(parent.value, 0, path, &length)) return 3;
    const auto filename = std::wcsrchr(path, L'\\');
    if (!filename || _wcsicmp(filename + 1, L"LogonUI.exe")) return 3;
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return 3;
    DWORD parent_session = 0;
    if (!ProcessIdToSessionId(parent_id, &parent_session) || parent_session != session) return 3;
    Handle mapping{OpenFileMappingW(FILE_MAP_READ, FALSE, argv[1])};
    if (!mapping.value) return 4;
    const auto* shared = static_cast<const su::status::SharedState*>(
        MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, sizeof(su::status::SharedState)));
    if (!shared) return 4;
    if (shared->magic != 0x53325553 || shared->version != 1) { UnmapViewOfFile(shared); return 4; }
    if (!ui_access_child && !has_ui_access()) {
        if (const auto result = run_ui_access_display(parent.value)) {
            UnmapViewOfFile(shared);
            return static_cast<int>(*result);
        }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ULONG_PTR gdiplus = 0;
    const Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&gdiplus, &input, nullptr) != Gdiplus::Ok) { UnmapViewOfFile(shared); return 5; }
    WNDCLASSW type{};
    type.lpfnWndProc = procedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = window_class;
    const auto window_atom = RegisterClassW(&type);
    if (!window_atom) { Gdiplus::GdiplusShutdown(gdiplus); UnmapViewOfFile(shared); return 5; }
    {
        const auto original_desktop = GetThreadDesktop(GetCurrentThreadId());
        HDESK display_desktop = nullptr;
        std::unique_ptr<Display> display;
        std::wstring selected_desktop;
        bool done = false;
        while (!done && WaitForSingleObject(parent.value, 0) == WAIT_TIMEOUT) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) { done = true; break; }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            const auto now = GetTickCount64();
            const auto heartbeat = shared->heartbeat.load(std::memory_order_acquire);
            if (now < heartbeat || now - heartbeat > 5000) break;
            const auto input = OpenInputDesktop(0, FALSE,
                DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_WRITEOBJECTS);
            const auto name = desktop_name(input);
            const auto allowed = _wcsicmp(name.c_str(), L"Winlogon") == 0
                || (_wcsicmp(name.c_str(), L"Default") == 0 && session_locked(session));
            if (!allowed || now - heartbeat > 1500) {
                if (display) display->hide();
            } else {
                if (!display || _wcsicmp(name.c_str(), selected_desktop.c_str()) != 0) {
                    // SetThreadDesktop requires a thread with no windows or
                    // hooks. Destroy all display objects before changing it.
                    display.reset();
                    if (SetThreadDesktop(input)) {
                        if (display_desktop) CloseDesktop(display_desktop);
                        display_desktop = input;
                        selected_desktop = name;
                        display = std::make_unique<Display>(window_atom);
                    }
                }
                if (display) display->update(su::status::decode(shared->state.load(std::memory_order_acquire)), now);
            }
            if (input && input != display_desktop) CloseDesktop(input);
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 33, QS_ALLINPUT);
        }
        display.reset();
        SetThreadDesktop(original_desktop);
        if (display_desktop) CloseDesktop(display_desktop);
    }
    UnregisterClassW(window_class, GetModuleHandleW(nullptr));
    Gdiplus::GdiplusShutdown(gdiplus);
    UnmapViewOfFile(shared);
    return 0;
}
