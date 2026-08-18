#include "app_window.h"

import std;
import su.app.controller;
import su.app.i18n;
import su.app.user;
import su.core.types;
import su.app.preview;
import su.app.preferences;
import su.app.session;
import su.app.theme;

#ifdef _WIN32
extern "C" int su_win_enum_own_windows(char* out, size_t cap);
extern "C" int su_win_find_window(const char* needle, char* out, size_t cap);
extern "C" int su_win_count_own_threads(void);
extern "C" unsigned long __stdcall GetCurrentThreadId(void);
extern "C" int __stdcall GetEnvironmentVariableW(const wchar_t* name, wchar_t* buffer, unsigned long size);
extern "C" int __stdcall SetEnvironmentVariableW(const wchar_t* name, const wchar_t* value);
extern "C" int __stdcall GetUserDefaultLocaleName(wchar_t* locale_name, int locale_name_count);
extern "C" unsigned long __stdcall GetLastError(void);
extern "C" int __stdcall SystemParametersInfoW(unsigned int action, unsigned int param, void* value, unsigned int win_ini);
extern "C" void* __stdcall LoadImageW(void* instance, const wchar_t* name, unsigned int type, int width, int height, unsigned int flags);
extern "C" unsigned long long __stdcall SendMessageW(void* window, unsigned int message, unsigned long long wparam, long long lparam);
extern "C" void* __stdcall FindWindowW(const wchar_t* class_name, const wchar_t* window_name);
extern "C" unsigned long __stdcall GetModuleFileNameW(void* module, wchar_t* buffer, unsigned long size);
extern "C" void __stdcall DestroyIcon(void* icon);
#endif

namespace {

namespace ui = su::app::ui;

using ProfileModel = slint::VectorModel<ui::ProfileRow>;
using DeploymentTargetModel = slint::VectorModel<ui::DeploymentTargetRow>;
using WindowHandle = slint::ComponentHandle<ui::AppWindow>;
using WeakWindowHandle = slint::ComponentWeakHandle<ui::AppWindow>;

constexpr std::string_view xdg_app_id = "smile2unlock";

std::string camera_summary(const su::app::AppSnapshot& snapshot) {
    if (snapshot.cameras.size() == 1) {
        return snapshot.cameras.front().name;
    }
    return {};
}

slint::Color slint_color(su::app::ThemeColor color) {
    return slint::Color::from_argb_uint8(color.alpha, color.red, color.green, color.blue);
}

void apply_theme(const WindowHandle& window, const su::app::AppTheme& snapshot) {
    const auto& theme = window->global<ui::UiTheme>();
    theme.set_canvas(slint_color(snapshot.canvas));
    theme.set_surface(slint_color(snapshot.surface));
    theme.set_surface_subtle(slint_color(snapshot.surface_subtle));
    theme.set_surface_selected(slint_color(snapshot.surface_selected));
    theme.set_border(slint_color(snapshot.border));
    theme.set_divider(slint_color(snapshot.divider));
    theme.set_text_primary(slint_color(snapshot.text_primary));
    theme.set_text_secondary(slint_color(snapshot.text_secondary));
    theme.set_text_tertiary(slint_color(snapshot.text_tertiary));
    theme.set_primary(slint_color(snapshot.primary));
    theme.set_primary_hover(slint_color(snapshot.primary_hover));
    theme.set_primary_pressed(slint_color(snapshot.primary_pressed));
    theme.set_on_primary(slint_color(snapshot.on_primary));
    theme.set_success_surface(slint_color(snapshot.success_surface));
    theme.set_success_text(slint_color(snapshot.success_text));
    theme.set_warning_surface(slint_color(snapshot.warning_surface));
    theme.set_warning_text(slint_color(snapshot.warning_text));
    theme.set_danger_surface(slint_color(snapshot.danger_surface));
    theme.set_danger_hover(slint_color(snapshot.danger_hover));
    theme.set_danger_text(slint_color(snapshot.danger_text));
    theme.set_disabled_surface(slint_color(snapshot.disabled_surface));
    theme.set_disabled_text(slint_color(snapshot.disabled_text));
    theme.set_preview_surface(slint_color(snapshot.preview_surface));
    theme.set_preview_overlay(slint_color(snapshot.preview_overlay));
    theme.set_preview_text(slint_color(snapshot.preview_text));
    theme.set_face_indicator(slint_color(snapshot.face_indicator));
    theme.set_dark_mode(snapshot.mode == su::app::ThemeMode::dark);
}

void log_theme_diagnostics(const std::vector<std::string>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::println(stderr, "[theme] {}", diagnostic);
    }
}

std::string username_initial(std::string_view username) {
    if (username.empty()) {
        return "U";
    }
    auto initial = static_cast<char>(std::toupper(static_cast<unsigned char>(username.front())));
    return std::string(1, initial);
}

std::string enrollment_date(std::uint64_t created_at_unix) {
    if (created_at_unix > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return "-";
    }

    using namespace std::chrono;
    const auto timestamp = sys_seconds{seconds{static_cast<std::int64_t>(created_at_unix)}};
    const auto date = year_month_day{floor<days>(timestamp)};
    if (!date.ok()) {
        return "-";
    }
    return std::format(
        "{:04}-{:02}-{:02}",
        static_cast<int>(date.year()),
        static_cast<unsigned>(date.month()),
        static_cast<unsigned>(date.day()));
}

std::vector<ui::ProfileRow> profile_rows(
    const std::vector<su::app::FaceProfileSummary>& profiles) {
    std::vector<ui::ProfileRow> rows;
    rows.reserve(profiles.size());
    for (const auto& profile : profiles) {
        rows.push_back(ui::ProfileRow{
            .id = slint::SharedString(profile.id),
            .label = slint::SharedString(profile.label),
            .enrolled_at = slint::SharedString(enrollment_date(profile.created_at_unix)),
        });
    }
    return rows;
}

void update_profiles(
    const std::shared_ptr<ProfileModel>& model,
    const std::vector<su::app::FaceProfileSummary>& profiles) {
    model->set_vector(profile_rows(profiles));
}

std::vector<ui::DeploymentTargetRow> deployment_target_rows(
    const std::vector<su::app::DeploymentTargetStatus>& targets) {
    auto rows = std::vector<ui::DeploymentTargetRow>{};
    rows.reserve(targets.size());
    for (const auto& target : targets) {
        rows.push_back(ui::DeploymentTargetRow{
            .id = slint::SharedString(target.id),
            .service = slint::SharedString(target.service),
            .effective_path = slint::SharedString(target.effective_path),
            .role = slint::SharedString(target.role),
            .state = slint::SharedString(target.state),
            .password_fallback = target.password_fallback,
            .configured = target.configured,
            .configurable = target.configurable,
            .managed = target.managed,
            .wallet_available = target.wallet_available,
            .wallet_enabled = target.wallet_enabled,
        });
    }
    return rows;
}

void set_activity(
    const WindowHandle& window,
    std::string_view title,
    std::string_view detail,
    std::string_view tone,
    bool busy = false) {
    window->set_activity_title(slint::SharedString(title));
    window->set_activity_detail(slint::SharedString(detail));
    window->set_activity_tone(slint::SharedString(tone));
    window->set_busy(busy);
}

// Diagnostic log for GUI bring-up (window creation, GL, event loop). File
// based because GUI-subsystem builds have no console; fail-silent.
void gui_log(const std::string& message) {
    std::error_code error;
    const auto path = std::filesystem::temp_directory_path(error) / "su_gui.log";
    if (!error) {
        if (auto file = std::ofstream(path, std::ios::app)) {
            file << message << "\n";
        }
    }
}

#ifdef _WIN32
extern "C" unsigned long __stdcall GetCurrentThreadId(void);
#endif

double now_seconds() {
    namespace chrono = std::chrono;
    return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count();
}

unsigned long thread_id() {
#ifdef _WIN32
    return ::GetCurrentThreadId();
#else
    return static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

// Timed + thread-id variant so we can tell WHICH thread wrote a line and
// measure how long each stage took (a stuck thread stops producing lines).
void gui_log_t(const std::string& message) {
    gui_log(std::format("[{:>9.3f}s tid=0x{:X}] {}", now_seconds(), thread_id(), message));
}

#ifdef _WIN32
// Set the title-bar and taskbar icon from the .ico file next to the
// executable. slint 1.17 cannot embed Window.icon (the compiler emits
// Image::load_from_path with the build machine's absolute path), so apply
// the icon directly via WM_SETICON once the native window exists. Call on
// the event-loop thread (e.g. from a timer callback).
void apply_window_icon() {
    wchar_t exe_path[260] = {};
    if (::GetModuleFileNameW(nullptr, exe_path, 260) == 0) {
        return;
    }
    std::wstring icon_path(exe_path);
    const auto slash = icon_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return;
    }
    icon_path.resize(slash + 1);
    icon_path += L"Smile2Unlock.ico";
    if (void* hwnd = ::FindWindowW(nullptr, L"Smile2Unlock"); hwnd != nullptr) {
        const auto set_icon = [hwnd, &icon_path](int size, unsigned long long which) {
            if (void* icon = ::LoadImageW(
                    nullptr,
                    icon_path.c_str(),
                    1 /* IMAGE_ICON */,
                    size,
                    size,
                    0x10 /* LR_LOADFROMFILE */);
                icon != nullptr) {
                ::SendMessageW(hwnd, 0x80 /* WM_SETICON */, which, reinterpret_cast<long long>(icon));
            }
        };
        set_icon(32, 1 /* ICON_BIG: taskbar / alt-tab */);
        set_icon(16, 0 /* ICON_SMALL: title bar */);
        gui_log_t("window icon applied");
    }
}
#endif

std::size_t language_index(const WindowHandle& window) {
    return static_cast<std::size_t>(std::max(window->get_language_index(), 0));
}

std::string translated(
    const su::app::LanguageCatalog& catalog,
    const WindowHandle& window,
    std::string_view key) {
    return catalog.translate(language_index(window), key);
}

std::string translated_value(
    const su::app::LanguageCatalog& catalog,
    const WindowHandle& window,
    std::string_view key,
    std::string_view value) {
    return catalog.translate_value(language_index(window), key, value);
}

std::string localized_backend_error(
    const su::app::LanguageCatalog& catalog,
    const WindowHandle& window,
    std::string_view error) {
    const auto key = error == "Windows password verification failed"
            || error == "Windows password is invalid"
        ? "error.windows_password"
        : error == "secure profile store is unavailable"
            ? "error.secure_store_unavailable"
        : error == "Smile2Unlock auth service returned an invalid response"
            ? "error.auth_service_response"
        : error == "auth service denied profile access"
            ? "error.profile_access_denied"
        : error == "face profile was not found"
            ? "error.profile_not_found"
        : std::string_view{};
    return key.empty() ? std::string(error) : translated(catalog, window, key);
}

void set_preview_idle(
    const WindowHandle& window,
    const su::app::LanguageCatalog& catalog) {
    window->set_preview_status_text(slint::SharedString(translated(catalog, window, "preview.idle")));
    window->set_face_box_visible(false);
}

std::filesystem::path executable_directory(const char* argument_zero) {
    std::error_code error;
    auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        return executable.parent_path();
    }
    executable = std::filesystem::absolute(argument_zero, error);
    return error ? std::filesystem::current_path() : executable.parent_path();
}

std::filesystem::path language_directory(const std::filesystem::path& executable_dir) {
    // Walk up from the executable looking for assets/i18n, matching the
    // model-dir lookup, so both flat (exe + assets side by side) and
    // bin/assets layouts resolve. Falls back to the Linux system location.
    auto directory = executable_dir;
    while (true) {
        auto error = std::error_code{};
        const auto candidate = directory / "assets" / "i18n";
        if (std::filesystem::is_directory(candidate, error)) {
            return candidate;
        }
        if (!directory.has_parent_path() || directory == directory.parent_path()) {
            break;
        }
        directory = directory.parent_path();
    }
    return executable_dir / ".." / "share" / "smile2unlock" / "i18n";
}

std::filesystem::path ui_preference_path() {
#ifdef _WIN32
    if (const auto* appdata = std::getenv("APPDATA");
        appdata != nullptr && *appdata != '\0') {
        return std::filesystem::path(appdata) / "smile2unlock" / "ui.json";
    }
    if (const auto* profile = std::getenv("USERPROFILE");
        profile != nullptr && *profile != '\0') {
        return std::filesystem::path(profile) / "AppData" / "Roaming"
            / "smile2unlock" / "ui.json";
    }
#endif
    if (const auto* config_home = std::getenv("XDG_CONFIG_HOME");
        config_home != nullptr && *config_home != '\0') {
        return std::filesystem::path(config_home) / "smile2unlock" / "ui.json";
    }
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".config" / "smile2unlock" / "ui.json";
    }
    return std::filesystem::current_path() / ".smile2unlock-ui.json";
}

std::string system_locale() {
#ifdef _WIN32
    auto locale_name = std::array<wchar_t, 85>{};
    if (::GetUserDefaultLocaleName(
            locale_name.data(), static_cast<int>(locale_name.size())) > 0) {
        const auto locale = std::wstring_view{locale_name.data()};
        if (locale.starts_with(L"zh")) {
            return "zh-CN";
        }
    }
    return "en";
#else
    for (const auto* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        if (const auto* value = std::getenv(name); value != nullptr && *value != '\0') {
            return value;
        }
    }
    return "en";
#endif
}

int theme_preference_index(su::app::ThemePreference preference) {
    switch (preference) {
        case su::app::ThemePreference::system:
            return 0;
        case su::app::ThemePreference::light:
            return 1;
        case su::app::ThemePreference::dark:
            return 2;
    }
    return 0;
}

std::optional<su::app::ThemePreference> theme_preference_at(int index) {
    switch (index) {
        case 0:
            return su::app::ThemePreference::system;
        case 1:
            return su::app::ThemePreference::light;
        case 2:
            return su::app::ThemePreference::dark;
        default:
            return std::nullopt;
    }
}

int window_controls_preference_index(su::app::WindowControlsPreference preference) {
    switch (preference) {
        case su::app::WindowControlsPreference::automatic:
            return 0;
        case su::app::WindowControlsPreference::visible:
            return 1;
        case su::app::WindowControlsPreference::hidden:
            return 2;
    }
    return 0;
}

std::optional<su::app::WindowControlsPreference> window_controls_preference_at(int index) {
    switch (index) {
        case 0:
            return su::app::WindowControlsPreference::automatic;
        case 1:
            return su::app::WindowControlsPreference::visible;
        case 2:
            return su::app::WindowControlsPreference::hidden;
        default:
            return std::nullopt;
    }
}

void persist_ui_preferences(
    const std::filesystem::path& path,
    const su::app::UiPreferences& preferences) {
    if (const auto saved = su::app::save_ui_preferences(path, preferences); !saved) {
        std::println(stderr, "[preferences] {}", saved.error());
    }
}

void apply_system_status(
    const WindowHandle& window,
    const su::app::SystemStatus& status,
    const std::shared_ptr<DeploymentTargetModel>& deployment_targets) {
    window->set_service_available(status.service_available);
    window->set_account_credential_configured(status.account_credential_configured);
    window->set_storage_protection_index(static_cast<int>(status.storage_protection));
    window->set_pam_status_known(status.pam_status_known);
    window->set_pam_configured(status.pam_configured);
    window->set_pam_service(slint::SharedString(status.pam_service));
    window->set_deployment_helper_available(status.deployment_helper_available);
    gui_log_t(std::format("deployment helper available: {}",
        status.deployment_helper_available ? "yes" : "no"));
    window->set_deployment_installer_available(status.deployment_installer_available);
    window->set_login_pam_configured(status.login_pam_configured);
    window->set_lock_pam_configured(status.lock_pam_configured);
    deployment_targets->set_vector(deployment_target_rows(status.deployment_targets));
}

using DeploymentOperation = std::function<std::expected<std::string, std::string>()>;

void start_deployment_operation(
    const WeakWindowHandle& weak_window,
    const std::shared_ptr<su::app::AppController>& controller,
    const std::shared_ptr<DeploymentTargetModel>& deployment_targets,
    const std::shared_ptr<const su::app::LanguageCatalog>& catalog,
    std::string success_key,
    DeploymentOperation operation) {
    if (const auto window = weak_window.lock()) {
        (*window)->set_busy(true);
        (*window)->set_deployment_operation_status(slint::SharedString(
            translated(*catalog, *window, "deployment.operation_working")));
    }
    std::thread([
        weak_window,
        controller,
        deployment_targets,
        catalog,
        success_key = std::move(success_key),
        operation = std::move(operation)]() mutable {
        auto result = operation();
        auto status = controller->load_system_status();
        slint::invoke_from_event_loop([
            weak_window,
            deployment_targets,
            catalog,
            success_key = std::move(success_key),
            result = std::move(result),
            status = std::move(status)]() mutable {
            const auto window = weak_window.lock();
            if (!window) {
                return;
            }
            apply_system_status(*window, status, deployment_targets);
            (*window)->set_busy(false);
            (*window)->set_deployment_operation_status(slint::SharedString(
                result
                    ? translated(*catalog, *window, success_key)
                    : translated_value(
                        *catalog,
                        *window,
                        "deployment.operation_failed",
                        result.error())));
        });
    }).detach();
}

#ifndef _WIN32
// Resolve the desktop size on Linux without linking X11/Wayland client
// libraries (su_app is a Wayland-first GUI; adding libX11 would pull in a
// long display-manager dependency chain). Probe the compositor through CLI
// tools that ship alongside it:
//   - Wayland (sway/niri/labwc/…): `wlr-randr` (widely installed with wlr
//     compositors; falls back to nothing if missing)
//   - X11: `xdpyinfo`
// Returns false when no probe yields a size; the caller keeps the design size.
bool detect_linux_desktop_size(float& width, float& height) {
    const bool wayland = std::getenv("WAYLAND_DISPLAY") != nullptr;
    const bool x11 = std::getenv("DISPLAY") != nullptr;
    if (!wayland && !x11) {
        return false;
    }
    auto run = [](const std::string& cmd) {
        std::array<char, 4096> buffer{};
        if (FILE* pipe = ::popen(cmd.c_str(), "r")) {
            std::string text;
            std::size_t n = 0;
            while ((n = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
                text.append(buffer.data(), n);
            }
            ::pclose(pipe);
            return text;
        }
        return std::string{};
    };
    if (wayland) {
        // wlr-randr output lines like "HDMI-A-1 connected 2560x1600@...".
        // Take the largest WxH across all listed outputs (single-display and
        // the primary of a multi-head setup both land correctly).
        const auto out = run("wlr-randr");
        if (!out.empty()) {
            std::regex mode{R"((\d+)\s*x\s*(\d+))"};
            std::smatch match;
            std::size_t best_w = 0, best_h = 0;
            auto it = std::sregex_iterator(out.begin(), out.end(), mode);
            auto end = std::sregex_iterator{};
            for (; it != end; ++it) {
                try {
                    const auto w = std::stoul((*it)[1].str());
                    const auto h = std::stoul((*it)[2].str());
                    if (w * h > best_w * best_h) {
                        best_w = w;
                        best_h = h;
                    }
                } catch (const std::exception&) {
                    continue;
                }
            }
            if (best_w != 0 && best_h != 0) {
                width = static_cast<float>(best_w);
                height = static_cast<float>(best_h);
                return true;
            }
        }
    }
    if (x11) {
        // xdpyinfo: "dimensions:    2560x1600 pixels"
        const auto out = run("xdpyinfo");
        std::regex dimensions{R"(\b(\d+)x(\d+)\s+pixels)"};
        std::smatch match;
        if (std::regex_search(out, match, dimensions)) {
            try {
                width = static_cast<float>(std::stoul(match[1].str()));
                height = static_cast<float>(std::stoul(match[2].str()));
                return true;
            } catch (const std::exception&) {
                return false;
            }
        }
    }
    return false;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // GUI-subsystem builds have no console: capture stderr so Rust-side
    // diagnostics from slint (backend/renderer errors, panics) are visible.
    if (auto* log = std::freopen("C:/Windows/Temp/su_stderr.log", "w", stderr)) {
        (void)log;
    }
#endif
    gui_log_t("main enter");
#ifdef _WIN32
    // Force slint's software renderer. Two requirements:
    //  1. slint reads SLINT_BACKEND from the OS environment block (Rust
    //     std::env), not from the C runtime environ that mingw's _putenv_s
    //     updates; SetEnvironmentVariableW updates the OS block.
    //  2. Valid values in slint 1.17: gl|winit|femtovg|skia|sw|software.
    //     "software" selects the winit backend with the CPU renderer, so no
    //     GL context (Mesa) is created.
    if (::GetEnvironmentVariableW(L"SLINT_BACKEND", nullptr, 0) == 0) {
        ::SetEnvironmentVariableW(L"SLINT_BACKEND", L"software");
    }
#endif
    slint::set_xdg_app_id(xdg_app_id);
    const auto preference_path = ui_preference_path();
    gui_log_t("preference path: " + preference_path.string());
    auto preferences = std::make_shared<su::app::UiPreferences>();
    if (auto loaded = su::app::load_ui_preferences(preference_path); loaded) {
        *preferences = std::move(*loaded);
    } else {
        std::println(stderr, "[preferences] {}", loaded.error());
    }
    gui_log_t("preferences loaded");
    const auto theme_paths = su::app::default_theme_paths();
    const auto theme_commands = su::app::system_theme_command_runner();
    auto initial_theme = su::app::load_desktop_theme(
        theme_paths, theme_commands, preferences->theme);
    log_theme_diagnostics(initial_theme.diagnostics);
    gui_log_t("theme loaded");
    const auto language_path = language_directory(
        executable_directory(argc > 0 ? argv[0] : "su_app"));
    auto loaded_catalog = su::app::LanguageCatalog::load(language_path);
    if (!loaded_catalog) {
        std::cerr << "su_app failed to load language packs: " << loaded_catalog.error() << '\n';
        return 1;
    }
    gui_log_t("language loaded");
    const auto catalog = std::make_shared<const su::app::LanguageCatalog>(std::move(*loaded_catalog));
    const auto selected_language = catalog->select_language(preferences->language, system_locale());

    auto controller = std::make_shared<su::app::AppController>();
    gui_log_t("controller constructed");
    auto preview = std::make_shared<su::app::PreviewController>();
    const auto snapshot = controller->load_initial_snapshot();
    gui_log_t("snapshot loaded");
    if (!snapshot) {
        std::cerr << "su_app failed to start: " << snapshot.error() << '\n';
        return 1;
    }

    auto window = ui::AppWindow::create();
    gui_log_t("AppWindow::create ok");
    apply_theme(window, initial_theme.snapshot.theme);
    const WeakWindowHandle weak_window(window);
    const auto theme_monitor = std::make_shared<su::app::ThemeMonitor>(
        theme_paths,
        theme_commands,
        initial_theme.snapshot,
        preferences->theme,
        [weak_window](su::app::ThemeLoadResult loaded) {
            log_theme_diagnostics(loaded.diagnostics);
            slint::invoke_from_event_loop(
                [weak_window, snapshot = std::move(loaded.snapshot)] {
                    if (const auto window = weak_window.lock()) {
                        apply_theme(*window, snapshot.theme);
                    }
                });
        });
    const auto profiles = std::make_shared<ProfileModel>(profile_rows(snapshot->profiles));
    const auto deployment_targets = std::make_shared<DeploymentTargetModel>();
    const auto session_lock_monitor = std::make_unique<su::app::SessionLockMonitor>(
        [weak_window, controller, preview, catalog] {
            slint::invoke_from_event_loop([weak_window, controller, preview, catalog] {
                const auto preview_was_running = preview->is_running();
                preview->stop();
                controller->cancel_camera_operation();
                if (preview_was_running) {
                    const auto window = weak_window.lock();
                    if (!window) {
                        return;
                    }
                    set_preview_idle(*window, *catalog);
                }
                std::println(stderr, "[session] GUI camera released for session lock");
            });
        });

    window->on_translate([catalog](slint::SharedString key, int index) {
        return slint::SharedString(catalog->translate(
            static_cast<std::size_t>(std::max(index, 0)), std::string_view(key)));
    });
    window->on_translate_value(
        [catalog](slint::SharedString key, int index, slint::SharedString value) {
            return slint::SharedString(catalog->translate_value(
                static_cast<std::size_t>(std::max(index, 0)),
                std::string_view(key),
                std::string_view(value)));
        });
    std::vector<slint::SharedString> language_names;
    for (const auto& name : catalog->language_names()) {
        language_names.emplace_back(name);
    }
    window->set_language_names(
        std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(language_names)));
    window->set_language_index(static_cast<int>(selected_language));
    window->set_theme_mode_index(theme_preference_index(preferences->theme));
    window->set_window_controls_index(
        window_controls_preference_index(preferences->window_controls));
    window->set_window_frame_visible(su::app::window_controls_visible(
        preferences->window_controls, theme_paths.desktop));
    window->on_language_selected(
        [weak_window, preview, catalog, preference_path, preferences](int index) {
            if (index < 0 || static_cast<std::size_t>(index) >= catalog->size()) {
                return;
            }
            preferences->language = catalog->language_code(static_cast<std::size_t>(index));
            persist_ui_preferences(preference_path, *preferences);
            if (const auto window = weak_window.lock()) {
                if (!preview->is_running()) {
                    set_preview_idle(*window, *catalog);
                }
                if ((*window)->get_desktop_auth_passed()) {
                    (*window)->set_activity_title(slint::SharedString(
                        translated(*catalog, *window, "activity.auth_passed")));
                }
            }
        });
    window->on_theme_mode_selected(
        [preference_path, preferences, theme_monitor](int index) {
            const auto selected = theme_preference_at(index);
            if (!selected) {
                return;
            }
            preferences->theme = *selected;
            persist_ui_preferences(preference_path, *preferences);
            theme_monitor->set_theme_preference(*selected);
        });
    window->on_window_controls_selected(
        [weak_window, preference_path, preferences, desktop = theme_paths.desktop](int index) {
            const auto selected = window_controls_preference_at(index);
            if (!selected) {
                return;
            }
            preferences->window_controls = *selected;
            persist_ui_preferences(preference_path, *preferences);
            if (const auto window = weak_window.lock()) {
                (*window)->set_window_frame_visible(
                    su::app::window_controls_visible(*selected, desktop));
            }
        });

    std::vector<slint::SharedString> camera_names;
    std::vector<int> camera_indices;
    camera_names.reserve(snapshot->cameras.size());
    camera_indices.reserve(snapshot->cameras.size());
    auto selected_camera = 0;
    for (const auto& camera : snapshot->cameras) {
        if (camera.index == snapshot->config.selected_camera) {
            selected_camera = static_cast<int>(camera_indices.size());
        }
        camera_names.emplace_back(std::format("{}: {}", camera.index, camera.name));
        camera_indices.push_back(camera.index);
    }

    const auto username = su::app::current_username(
        catalog->translate(selected_language, "common.current_user"));
    // Window title: plain product name (the versioned string stays in
    // snapshot->title for the diagnostics view).
    window->set_title_text(slint::SharedString("Smile2Unlock"));
    window->set_username(slint::SharedString(username));
    window->set_username_initial(slint::SharedString(username_initial(username)));
    window->set_core_version(slint::SharedString(catalog->translate_value(
        selected_language,
        "diagnostics.core_version",
#ifdef SU_VERSION_STR
        SU_VERSION_STR
#else
        std::format("{}", su::app::core_version_major())
#endif
        )));
    window->set_config_path_text(slint::SharedString(snapshot->config_path));
    window->set_profile_store_path_text(slint::SharedString(snapshot->profile_store_path));
    window->set_profiles(profiles);
    window->set_deployment_targets(deployment_targets);
    window->set_camera_options(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(camera_names)));
    window->set_camera_text(slint::SharedString(camera_summary(*snapshot)));
    window->set_camera_count(static_cast<int>(snapshot->cameras.size()));
    gui_log_t(std::format("cameras enumerated: {} (first='{}')",
        snapshot->cameras.size(),
        snapshot->cameras.empty() ? "<none>" : snapshot->cameras.front().name));
    window->set_seetaface_available(snapshot->seetaface_available);
    apply_system_status(window, controller->load_system_status(), deployment_targets);
    window->set_desktop_auth_passed(preferences->desktop_auth_test_passed);
    window->set_selected_camera(selected_camera);
    window->set_recognition_threshold(snapshot->config.recognition_threshold);
    window->set_liveness_enabled(snapshot->config.liveness_detection);
    window->set_liveness_threshold(snapshot->config.liveness_threshold);
    window->set_preview_fps(static_cast<int>(snapshot->config.preview_fps));
    // Recognition trigger policy (Windows only; the UI hides these on Linux,
    // but the values are still carried in the config struct everywhere).
    window->set_recognition_mode(static_cast<int>(snapshot->config.recognition_mode));
    window->set_auto_delay_sec(static_cast<int>(snapshot->config.auto_delay_sec));
    window->set_retry_delay_sec(static_cast<int>(snapshot->config.retry_delay_sec));
    window->set_timeout_sec(static_cast<int>(snapshot->config.timeout_sec));
#ifdef _WIN32
    window->set_platform_windows(true);
#endif
    set_preview_idle(window, *catalog);
    window->set_activity_title(slint::SharedString(catalog->translate(
        selected_language,
        preferences->desktop_auth_test_passed
            ? "activity.auth_passed"
            : "activity.not_checked")));
    window->set_settings_status(slint::SharedString(catalog->translate(selected_language, "settings.saved")));

    window->on_refresh_system_status_requested([weak_window, controller, deployment_targets] {
        std::thread([weak_window, controller, deployment_targets] {
            const auto status = controller->load_system_status();
            slint::invoke_from_event_loop([weak_window, deployment_targets, status] {
                if (const auto window = weak_window.lock()) {
                    apply_system_status(*window, status, deployment_targets);
                }
            });
        }).detach();
    });

    window->on_initialize_system_deployment_requested(
        [weak_window, controller, deployment_targets, catalog] {
            start_deployment_operation(
                weak_window,
                controller,
                deployment_targets,
                catalog,
                "deployment.initialize_success",
                [controller] { return controller->initialize_system_deployment(); });
        });

    window->on_install_deployment_helper_requested(
        [weak_window, controller, deployment_targets, catalog] {
            start_deployment_operation(
                weak_window,
                controller,
                deployment_targets,
                catalog,
                "deployment.install_helper_success",
                [controller] { return controller->install_deployment_helper(); });
        });

    window->on_configure_desktop_target_requested(
        [weak_window, controller, deployment_targets, catalog](
            slint::SharedString target,
            bool wallet_token) {
            start_deployment_operation(
                weak_window,
                controller,
                deployment_targets,
                catalog,
                "deployment.configure_success",
                [controller, target = std::string(target), wallet_token] {
                    return controller->configure_desktop_target(target, wallet_token);
                });
        });

    window->on_rollback_desktop_target_requested(
        [weak_window, controller, deployment_targets, catalog](slint::SharedString target) {
            start_deployment_operation(
                weak_window,
                controller,
                deployment_targets,
                catalog,
                "deployment.rollback_success",
                [controller, target = std::string(target)] {
                    return controller->rollback_desktop_target(target);
                });
        });

    window->on_configure_account_credential_requested(
        [weak_window, controller, deployment_targets, catalog](
            slint::SharedString requested_password) {
            if (const auto window = weak_window.lock()) {
                (*window)->set_busy(true);
                set_activity(
                    *window,
                    translated(*catalog, *window, "account.credential_saving"),
                    translated(*catalog, *window, "account.credential_verifying"),
                    "warn",
                    true);
            }
            auto password = std::string(requested_password);
            std::thread([
                weak_window,
                controller,
                deployment_targets,
                catalog,
                password = std::move(password)]() mutable {
                const auto configured =
                    controller->configure_windows_account_credential(password);
                std::fill(password.begin(), password.end(), '\0');
                password.clear();
                auto status = controller->load_system_status();
                slint::invoke_from_event_loop([
                    weak_window,
                    deployment_targets,
                    catalog,
                    configured,
                    status = std::move(status)]() mutable {
                    const auto window = weak_window.lock();
                    if (!window) {
                        return;
                    }
                    apply_system_status(*window, status, deployment_targets);
                    (*window)->set_account_password_text("");
                    if (!configured) {
                        set_activity(
                            *window,
                            translated(*catalog, *window, "account.credential_save_failed"),
                            localized_backend_error(
                                *catalog, *window, configured.error()),
                            "bad");
                        return;
                    }
                    set_activity(
                        *window,
                        translated(*catalog, *window, "account.credential_saved"),
                        translated(*catalog, *window, "account.credential_saved_detail"),
                        "good");
                });
            }).detach();
        });

    window->on_enroll_current_frame_requested(
        [weak_window, controller, preview, profiles, catalog](
            slint::SharedString requested_label) {
            if (auto window = weak_window.lock()) {
                preview->stop();
                set_preview_idle(*window, *catalog);
                set_activity(
                    *window,
                    translated(*catalog, *window, "activity.capturing_face"),
                    translated(*catalog, *window, "activity.look_camera"),
                    "warn",
                    true);
            }

            auto label = std::string(requested_label);
            if (label.empty()) {
                if (const auto window = weak_window.lock()) {
                    label = translated(*catalog, *window, "enrollment.default_label");
                }
            }
            std::thread([weak_window, controller, profiles, catalog,
                         label = std::move(label)]() mutable {
                const auto enrolled = controller->enroll_face_profile_from_current_frame(label);
                auto rows = enrolled
                    ? controller->list_face_profile_rows()
                    : std::expected<std::vector<su::app::FaceProfileSummary>, std::string>{
                        std::unexpected(enrolled.error())};

                slint::invoke_from_event_loop(
                    [weak_window, profiles, catalog, label, rows = std::move(rows)]() mutable {
                        const auto window = weak_window.lock();
                        if (!window) {
                            return;
                        }
                        if (!rows) {
                            set_activity(
                                *window,
                                translated(*catalog, *window, "activity.enrollment_failed"),
                                localized_backend_error(*catalog, *window, rows.error()),
                                "bad");
                            return;
                        }
                        update_profiles(profiles, *rows);
                        set_activity(
                            *window,
                            translated(*catalog, *window, "activity.enrolled"),
                            translated_value(*catalog, *window, "activity.enrolled_detail", label),
                            "good");
                    });
            }).detach();
        });

    window->on_current_frame_auth_requested([
        weak_window,
        controller,
        preview,
        profiles,
        catalog,
        preference_path,
        preferences] {
        if (auto window = weak_window.lock()) {
            preview->stop();
            set_preview_idle(*window, *catalog);
            set_activity(
                *window,
                translated(*catalog, *window, "activity.checking_identity"),
                translated(*catalog, *window, "activity.keep_centered"),
                "warn",
                true);
        }

        std::thread([
            weak_window,
            controller,
            profiles,
            catalog,
            preference_path,
            preferences] {
            auto result = controller->authenticate_current_frame();
            slint::invoke_from_event_loop(
                [
                    weak_window,
                    profiles,
                    catalog,
                    preference_path,
                    preferences,
                    result = std::move(result)]() mutable {
                    const auto window = weak_window.lock();
                    if (!window) {
                        return;
                    }
                    if (!result) {
                        set_activity(
                            *window,
                            translated(*catalog, *window, "activity.auth_unavailable"),
                            localized_backend_error(*catalog, *window, result.error()),
                            "bad");
                        return;
                    }

                    update_profiles(profiles, result->profiles);
                    if (result->decision.accepted) {
                        (*window)->set_desktop_auth_passed(true);
                        preferences->desktop_auth_test_passed = true;
                        persist_ui_preferences(preference_path, *preferences);
                    }
                    const auto detail = result->report.best_profile_label.empty()
                        ? translated_value(
                            *catalog,
                            *window,
                            "activity.auth_score",
                            std::format("{:.2f}", result->report.score))
                        : translated_value(
                            *catalog,
                            *window,
                            "activity.auth_matched",
                            std::format(
                                "{} ({:.2f})",
                                result->report.best_profile_label,
                                result->report.score));
                    set_activity(
                        *window,
                        translated(
                            *catalog,
                            *window,
                            result->decision.accepted
                                ? "activity.auth_passed"
                                : "activity.auth_rejected"),
                        detail,
                        result->decision.accepted ? "good" : "bad");
                });
        }).detach();
    });

    window->on_delete_profile_requested(
        [weak_window, controller, profiles, catalog](
            slint::SharedString profile_id) {
            const auto deleted = controller->delete_face_profile_by_id(
                std::string(profile_id));
            const auto window = weak_window.lock();
            if (!window) {
                return;
            }
            if (!deleted) {
                set_activity(
                    *window,
                    translated(*catalog, *window, "activity.remove_failed"),
                    localized_backend_error(*catalog, *window, deleted.error()),
                    "bad");
                return;
            }
            if (!*deleted) {
                set_activity(
                    *window,
                    translated(*catalog, *window, "activity.sample_not_found"),
                    translated(*catalog, *window, "activity.refresh_retry"),
                    "bad");
                return;
            }

            const auto rows = controller->list_face_profile_rows();
            if (!rows) {
                set_activity(
                    *window,
                    translated(*catalog, *window, "activity.removed"),
                    translated(*catalog, *window, "activity.refresh_after_remove_failed"),
                    "warn");
                return;
            }
            update_profiles(profiles, *rows);
            set_activity(
                *window,
                translated(*catalog, *window, "activity.removed"),
                translated(*catalog, *window, "activity.removed_detail"),
                "good");
        });

    window->on_refresh_profiles_requested([weak_window, controller, profiles, catalog] {
        const auto rows = controller->list_face_profile_rows();
        const auto window = weak_window.lock();
        if (!window) {
            return;
        }
        if (!rows) {
            set_activity(
                *window,
                translated(*catalog, *window, "activity.refresh_failed"),
                localized_backend_error(*catalog, *window, rows.error()),
                "bad");
            return;
        }
        update_profiles(profiles, *rows);
        set_activity(
            *window,
            translated(*catalog, *window, "activity.refreshed"),
            translated(*catalog, *window, "activity.refreshed_detail"),
            "good");
    });

    window->on_save_settings_requested(
        [weak_window, controller, camera_indices, catalog](
            int camera_selection,
            float recognition_threshold,
            bool liveness_enabled,
            float liveness_threshold,
            int preview_fps,
            int recognition_mode,
            int auto_delay_sec,
            int retry_delay_sec,
            int timeout_sec) {
            const auto window = weak_window.lock();
            if (!window) {
                return;
            }

            auto config = controller->load_config_snapshot();
            if (!config) {
                (*window)->set_settings_status(slint::SharedString(config.error()));
                set_activity(
                    *window,
                    translated(*catalog, *window, "activity.settings_unavailable"),
                    config.error(),
                    "bad");
                return;
            }
            if (camera_selection >= 0
                && static_cast<std::size_t>(camera_selection) < camera_indices.size()) {
                config->selected_camera = camera_indices[static_cast<std::size_t>(camera_selection)];
            }
            config->recognition_threshold = recognition_threshold;
            config->liveness_detection = liveness_enabled;
            config->liveness_threshold = liveness_threshold;
            config->preview_fps = static_cast<std::uint32_t>(std::clamp(preview_fps, 1, 60));
            config->recognition_mode = static_cast<std::uint32_t>(recognition_mode <= 1 ? recognition_mode : 0);
            config->auto_delay_sec = static_cast<std::uint32_t>(std::clamp(auto_delay_sec, 0, 3600));
            config->retry_delay_sec = static_cast<std::uint32_t>(std::clamp(retry_delay_sec, 1, 3600));
            config->timeout_sec = static_cast<std::uint32_t>(std::clamp(timeout_sec, 5, 600));

            const auto saved = controller->save_config_snapshot(*config);
            if (!saved) {
                (*window)->set_settings_status(slint::SharedString(saved.error()));
                set_activity(
                    *window,
                    translated(*catalog, *window, "activity.settings_not_saved"),
                    saved.error(),
                    "bad");
                return;
            }
            (*window)->set_settings_status(slint::SharedString(
                translated(*catalog, *window, "settings.saved")));
            set_activity(
                *window,
                translated(*catalog, *window, "settings.saved"),
                translated(*catalog, *window, "activity.settings_applied"),
                "good");
        });

    // Preview frames arrive on Slint's event loop. Keep only a weak window
    // handle so stopping the UI also releases the capture callback cleanly.
    auto push_preview_frame = [weak_window, catalog](
                                  slint::Image image,
                                  su::app::PreviewOverlay overlay) {
        const auto window = weak_window.lock();
        if (!window) {
            return;
        }
        (*window)->set_preview_image(std::move(image));
        const auto status = overlay.status_text == "preview.face_liveness"
            ? translated_value(
                *catalog,
                *window,
                overlay.status_text,
                std::format("{:.2f}", overlay.liveness_score))
            : translated(*catalog, *window, overlay.status_text);
        (*window)->set_preview_status_text(slint::SharedString(status));
        if (overlay.face_box && overlay.source_width > 0 && overlay.source_height > 0) {
            (*window)->set_preview_source_width(overlay.source_width);
            (*window)->set_preview_source_height(overlay.source_height);
            (*window)->set_face_box_x(static_cast<float>(overlay.face_box->x));
            (*window)->set_face_box_y(static_cast<float>(overlay.face_box->y));
            (*window)->set_face_box_w(static_cast<float>(overlay.face_box->width));
            (*window)->set_face_box_h(static_cast<float>(overlay.face_box->height));
            (*window)->set_face_box_visible(true);
        } else {
            (*window)->set_face_box_visible(false);
        }
    };

    window->on_start_preview_requested(
        [weak_window, controller, preview, push_preview_frame, catalog] {
        if (preview->is_running()) {
            return;
        }
        const auto config = controller->load_config_snapshot();
        const auto window = weak_window.lock();
        if (!window) {
            return;
        }
        if (!config) {
            (*window)->set_preview_status_text(slint::SharedString(config.error()));
            return;
        }

        (*window)->set_preview_status_text(slint::SharedString(
            translated(*catalog, *window, "preview.starting")));
        preview->start(
            controller->recognizer(),
            config->selected_camera,
            static_cast<int>(config->preview_fps),
            config->liveness_detection,
            push_preview_frame);
        if (!preview->is_running()) {
            (*window)->set_preview_status_text(slint::SharedString(
                translated(*catalog, *window, "preview.start_failed")));
        }
    });

    window->on_stop_preview_requested([weak_window, preview, catalog] {
        preview->stop();
        if (const auto window = weak_window.lock()) {
            set_preview_idle(*window, *catalog);
        }
    });

    gui_log_t(std::format("before run: visible={} size={}x{}",
        window->window().is_visible(),
        static_cast<int>(window->window().size().width),
        static_cast<int>(window->window().size().height)));
    // Poll window state once a second while the event loop runs, to see
    // whether show() actually makes the window visible.
    slint::Timer state_timer(std::chrono::milliseconds(1000), [weak_window] {
        auto w = weak_window.lock();
        if (w.has_value()) {
            auto handle = *w;
            auto& win = handle->window();
            gui_log_t(std::format("tick: visible={} size={}x{}",
                win.is_visible(),
                static_cast<int>(win.size().width),
                static_cast<int>(win.size().height)));
        }
    });
    gui_log_t(std::format("renderer env: SLINT_BACKEND={}",
        std::getenv("SLINT_BACKEND") ? std::getenv("SLINT_BACKEND") : "(unset)"));
    window->show();
#ifdef _WIN32
    gui_log_t(std::format("show() returned (lastError=0x{:X})", ::GetLastError()));
#else
    gui_log_t("show() returned");
#endif
    // Fit the window to the desktop so it is never larger than the screen.
    // Windows: SPI_GETWORKAREA reports PHYSICAL pixels; set_size takes a
    // logical size, so the design size must be compared in physical units.
    // Linux: the compositor's reported mode is physical pixels too. No
    // minimum-size or aspect-ratio constraints are set: this only picks a
    // proportional initial size, the window stays freely resizable. A
    // one-shot timer re-applies the size after the first layout pass.
    const auto fit_window_to_screen = [window] {
        float avail_w = 0.0f;
        float avail_h = 0.0f;
        float center_x = 0.0f;
        float center_y = 0.0f;
#ifdef _WIN32
        struct SuRect {
            long left;
            long top;
            long right;
            long bottom;
        };
        SuRect work{};
        if (::SystemParametersInfoW(0x0030 /* SPI_GETWORKAREA */, 0, &work, 0) == 0) {
            return;
        }
        avail_w = static_cast<float>(work.right - work.left) - 24.0f;
        avail_h = static_cast<float>(work.bottom - work.top) - 48.0f;
        center_x = (work.left + work.right) / 2.0f;
        center_y = (work.top + work.bottom) / 2.0f;
#else
        if (!detect_linux_desktop_size(avail_w, avail_h)) {
            return;
        }
        // Physical resolution: leave a small margin so the window is not
        // edge-to-edge on a tiling compositor.
        avail_w -= 24.0f;
        avail_h -= 48.0f;
        center_x = 0.0f;
        center_y = 0.0f;
#endif
        const float sf = window->window().scale_factor();
        const float scale = std::clamp(
            std::min(1.0f, std::min(avail_w / (1200.0f * sf), avail_h / (780.0f * sf))),
            0.5f,
            1.0f);
        const auto fitted = slint::LogicalSize({1200.0f * scale, 780.0f * scale});
        window->window().set_size(fitted);
#ifdef _WIN32
        // Center the window on the work area: winit's default placement can
        // leave it partly off-screen when the initial size exceeds the
        // screen (e.g. 1560x880 logical at 125% scaling on a 1920x1080
        // console). set_position takes logical coordinates.
        center_x -= fitted.width * sf / 2.0f;
        center_y -= fitted.height * sf / 2.0f;
        window->window().set_position(slint::LogicalPosition({center_x / sf, center_y / sf}));
#endif
        gui_log_t(std::format("window fitted: avail={}x{} sf={:.2f} -> {}x{} physical {}x{}",
            static_cast<int>(avail_w),
            static_cast<int>(avail_h),
            sf,
            static_cast<int>(fitted.width),
            static_cast<int>(fitted.height),
            static_cast<int>(fitted.width * sf),
            static_cast<int>(fitted.height * sf)));
    };
    fit_window_to_screen();
    // One-shot: re-apply the fitted size once the first layout pass has
    // settled (the layout may otherwise grow the window back to the
    // preferred size). A repeating timer would fight manual resizing.
    // Also apply the window icon here: at this point the native window
    // exists and the event loop is running.
    slint::Timer::single_shot(std::chrono::milliseconds(100), [window, fit_window_to_screen] {
        fit_window_to_screen();
#ifdef _WIN32
        apply_window_icon();
#endif
    });
    // Independent watcher: enumerates this process's top-level windows every
    // second (Win32 only, no slint main-thread calls) to see whether the
    // winit window is actually created/visible. Also logs the real OS thread
    // count so we can verify this watcher thread is actually alive.
#ifdef _WIN32
    std::thread window_watcher([] {
        gui_log_t("watcher thread started");
        for (int i = 0; i < 120; ++i) {
            if (i > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            char buffer[2048] = {};
            gui_log_t("watch: before find_window");
            const int f = su_win_find_window("Smile2Unlock", buffer, sizeof(buffer));
            gui_log_t(std::format("watch: find_window -> {} ({})", f, buffer));
            gui_log_t("watch: before enum_own");
            const int n = su_win_enum_own_windows(buffer, sizeof(buffer));
            const int threads = su_win_count_own_threads();
            gui_log_t(std::format("watch tick {} ({} bytes, threads={}): {}", i, n, threads, buffer));
        }
        gui_log_t("watcher thread done");
    });
    window_watcher.detach();
#endif
    gui_log_t("calling run_event_loop");
    slint::run_event_loop();
    gui_log_t("run returned");
    preview->stop();
    gui_log_t("main exiting");
    return 0;
}
