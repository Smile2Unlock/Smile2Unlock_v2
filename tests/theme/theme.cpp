import std;
import su.app.preferences;
import su.app.theme;

namespace {

using su::app::ThemeColor;
using su::app::ThemeMode;
using su::app::ThemePreference;
using su::app::ThemeSource;
using su::app::WindowControlsPreference;

struct MaterialRole {
    std::string name;
    std::string dark;
    std::string light;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::vector<MaterialRole> material_roles(std::string primary = "#bbc3ff") {
    return {
        {"background", "#131318", "#fbf8ff"},
        {"surface", "#131318", "#fbf8ff"},
        {"surface_container", "#1f1f25", "#efedf4"},
        {"surface_container_high", "#29292f", "#e9e7ef"},
        {"surface_container_lowest", "#0d0e13", "#ffffff"},
        {"outline", "#90909a", "#767680"},
        {"outline_variant", "#46464f", "#c7c5d0"},
        {"primary", std::move(primary), "#525a92"},
        {"primary_container", "#3a4379", "#dee0ff"},
        {"on_primary", "#232c61", "#ffffff"},
        {"secondary_container", "#45465f", "#e0e1ff"},
        {"on_secondary_container", "#e0e1ff", "#191a2c"},
        {"tertiary_container", "#5b3f5f", "#f9d8ff"},
        {"on_tertiary_container", "#f9d8ff", "#241028"},
        {"on_surface", "#e4e1e9", "#1b1b21"},
        {"on_surface_variant", "#c7c5d0", "#46464f"},
        {"error", "#ffb4ab", "#ba1a1a"},
        {"error_container", "#93000a", "#ffdad6"},
        {"on_error_container", "#ffdad6", "#410002"},
    };
}

std::string dms_palette(
    std::string primary = "#bbc3ff",
    bool include_optional_roles = true,
    bool include_status_roles = true) {
    auto roles = material_roles(std::move(primary));
    if (!include_optional_roles) {
        std::erase_if(roles, [](const auto& role) {
            return role.name == "surface_container"
                || role.name == "surface_container_high"
                || role.name == "surface_container_lowest"
                || role.name == "outline_variant";
        });
    }
    if (!include_status_roles) {
        std::erase_if(roles, [](const auto& role) {
            return role.name.starts_with("secondary_")
                || role.name.starts_with("tertiary_");
        });
    }
    auto output = std::string{"{\"colors\":{"};
    for (const auto mode : {ThemeMode::dark, ThemeMode::light}) {
        if (output.back() != '{') {
            output += ',';
        }
        output += std::format("\"{}\":{{", mode == ThemeMode::dark ? "dark" : "light");
        auto first = true;
        for (const auto& role : roles) {
            if (!first) {
                output += ',';
            }
            first = false;
            output += std::format(
                "\"{}\":\"{}\"",
                role.name,
                mode == ThemeMode::dark ? role.dark : role.light);
        }
        output += '}';
    }
    output += "}}";
    return output;
}

std::string matugen_palette(std::string primary = "#bbc3ff") {
    const auto roles = material_roles(std::move(primary));
    auto output = std::string{"{\"colors\":{"};
    auto first = true;
    for (const auto& role : roles) {
        if (!first) {
            output += ',';
        }
        first = false;
        output += std::format(
            "\"{}\":{{\"dark\":{{\"color\":\"{}\"}},"
            "\"light\":{{\"color\":\"{}\"}}}}",
            role.name,
            role.dark,
            role.light);
    }
    output += "}}";
    return output;
}

ThemeColor color(std::string_view value) {
    const auto parsed = su::app::parse_theme_color(value);
    require(parsed.has_value(), "test color must be valid");
    return *parsed;
}

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    auto stream = std::ofstream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
    require(static_cast<bool>(stream), "failed to write test fixture");
}

void atomic_replace(const std::filesystem::path& path, std::string_view contents) {
    auto temporary = path;
    temporary += ".new";
    write_file(temporary, contents);
    std::filesystem::rename(temporary, path);
}

struct TestTree {
    std::filesystem::path root;

    ~TestTree() {
        auto error = std::error_code{};
        std::filesystem::remove_all(root, error);
    }
};

void test_color_and_mode_parsing() {
    const auto rgba = su::app::parse_theme_color("#80112233");
    require(rgba.has_value(), "ARGB color should parse");
    require(
        *rgba == ThemeColor{.alpha = 0x80, .red = 0x11, .green = 0x22, .blue = 0x33},
        "ARGB channels should retain their order");
    require(!su::app::parse_theme_color("112233"), "missing hash should fail");
    require(!su::app::parse_theme_color("#xyzxyz"), "invalid hex should fail");
    require(
        su::app::contrast_ratio(color("#00ffffff"), color("#ffffff")) == 1.0,
        "transparent foreground should be composited before contrast checks");
    require(su::app::parse_dms_mode("\"dark\"") == ThemeMode::dark, "JSON mode should parse");
    require(
        su::app::parse_dms_session_mode(R"({"isLightMode":true})") == ThemeMode::light,
        "session mode should parse");
}

std::string with_dank16(std::string palette) {
    const auto block = std::string{
        "\"dank16\":{"
        "\"color2\":{\"dark\":\"#7fff95\",\"light\":\"#266d32\"},"
        "\"color3\":{\"dark\":\"#ffd772\",\"light\":\"#8d6c04\"}},"};
    palette.insert(1, block);
    return palette;
}

void require_readable_roles(const su::app::AppTheme& theme, std::string_view label) {
    const auto readable = [&](ThemeColor foreground, ThemeColor background, double ratio) {
        return su::app::contrast_ratio(foreground, background) >= ratio;
    };
    const auto text_roles = {theme.text, theme.text_dim, theme.accent, theme.ok, theme.warn, theme.danger, theme.idle};
    for (const auto background : {theme.canvas, theme.group, theme.group_hover}) {
        for (const auto foreground : text_roles) {
            require(readable(foreground, background, 4.5), std::format("{}: text role must reach 4.5:1", label));
        }
    }
    require(readable(theme.text, theme.sidebar, 4.5), std::format("{}: sidebar text contrast", label));
    require(readable(theme.text_dim, theme.sidebar, 4.5), std::format("{}: sidebar dim text contrast", label));
    require(readable(theme.text_faint, theme.group, 3.0), std::format("{}: faint text contrast", label));
    require(readable(theme.on_accent, theme.accent, 4.5), std::format("{}: on-accent contrast", label));
    require(readable(theme.on_tonal, theme.tonal, 4.5), std::format("{}: on-tonal contrast", label));
    require(readable(theme.on_nav_selected, theme.nav_selected, 4.5), std::format("{}: nav contrast", label));
    require(readable(theme.ok, theme.ok_bg, 3.0), std::format("{}: ok on its tint", label));
    require(readable(theme.danger, theme.danger_bg, 3.0), std::format("{}: danger on its tint", label));
}

// Every role must come from the palette: with a palette unlike the built-in one,
// no AppTheme field may still equal its built-in fallback value.
void test_no_role_keeps_builtin_fallback() {
    const auto members = std::array{
        &su::app::AppTheme::canvas, &su::app::AppTheme::sidebar, &su::app::AppTheme::group,
        &su::app::AppTheme::group_hover, &su::app::AppTheme::inset, &su::app::AppTheme::outline,
        &su::app::AppTheme::divider, &su::app::AppTheme::text, &su::app::AppTheme::text_dim,
        &su::app::AppTheme::text_faint, &su::app::AppTheme::accent, &su::app::AppTheme::on_accent,
        &su::app::AppTheme::tonal, &su::app::AppTheme::on_tonal, &su::app::AppTheme::nav_selected,
        &su::app::AppTheme::on_nav_selected, &su::app::AppTheme::ok, &su::app::AppTheme::ok_bg,
        &su::app::AppTheme::warn, &su::app::AppTheme::warn_bg, &su::app::AppTheme::danger,
        &su::app::AppTheme::danger_bg, &su::app::AppTheme::idle, &su::app::AppTheme::idle_bg,
        &su::app::AppTheme::preview_surface, &su::app::AppTheme::preview_overlay,
        &su::app::AppTheme::preview_text, &su::app::AppTheme::face_indicator};
    constexpr auto field_count = sizeof(su::app::AppTheme) - sizeof(ThemeMode);
    static_assert(
        members.size() * sizeof(ThemeColor) == field_count,
        "a new AppTheme color must be added to the unmapped-role test");
    const auto palette = [](std::string_view a, std::string_view b, std::string_view c) {
        auto roles = std::string{};
        for (const auto mode : {"dark", "light"}) {
            if (!roles.empty()) {
                roles += ',';
            }
            roles += std::format(
                "\"{}\":{{\"background\":\"{}\",\"surface\":\"{}\","
                "\"surface_container_lowest\":\"{}\",\"surface_container_low\":\"{}\","
                "\"surface_container\":\"{}\",\"surface_container_high\":\"{}\","
                "\"outline\":\"#8a6f9a\",\"outline_variant\":\"#5a3f6a\","
                "\"primary\":\"#d27be3\",\"on_primary\":\"#3a0a4a\","
                "\"primary_container\":\"#6a2a7a\",\"on_primary_container\":\"#f6d6ff\","
                "\"secondary_container\":\"#4a3a6a\",\"on_secondary_container\":\"#e6d6ff\","
                "\"on_surface\":\"{}\",\"on_surface_variant\":\"#d9c6e3\","
                "\"error\":\"#ff8aa0\"}}",
                mode, a, a, b, b, c, c, "#f1e4f7");
        }
        return std::format("{{\"colors\":{{{}}}}}", roles);
    };
    for (const auto mode : {ThemeMode::dark, ThemeMode::light}) {
        const auto theme = su::app::parse_material_theme(
            palette("#1a0f20", "#241830", "#2f2140"), mode);
        require(theme.has_value(), "synthetic palette should parse");
        const auto fallback = su::app::built_in_theme(mode);
        for (const auto member : members) {
            require(
                theme.value().*member != fallback.*member,
                "an AppTheme role still equals the built-in fallback instead of the palette");
        }
    }
}

void test_palette_mapping_and_validation() {
    const auto dark = su::app::parse_material_theme(dms_palette(), ThemeMode::dark);
    require(dark.has_value(), "DMS palette should parse");
    require(dark->canvas == color("#131318"), "background role should map to canvas");
    require(dark->group == color("#1f1f25"), "surface container should map to group");
    require(dark->group_hover == color("#29292f"), "high container should map to group hover");
    require(dark->sidebar == color("#1f1f25"), "missing low container falls back to container");
    require(dark->accent == color("#bbc3ff"), "primary role should map to accent");
    require(dark->tonal == color("#3a4379"), "primary container should map to tonal");
    require(dark->nav_selected == color("#45465f"), "secondary container should map to nav selection");
    require(dark->outline == color("#46464f"), "outline variant should map to outline");
    require(dark->idle_bg == dark->group_hover, "idle background follows the elevated surface");
    require(dark->ok != color("#bbc3ff") && dark->ok != dark->warn, "status hues must be distinct");
    require_readable_roles(*dark, "dark");

    const auto light = su::app::parse_material_theme(dms_palette(), ThemeMode::light);
    require(light.has_value() && light->mode == ThemeMode::light, "light scheme should parse");
    require(light->accent == color("#525a92"), "light primary should be selected");
    require_readable_roles(*light, "light");

    // dank16: per-mode status hues are toned down, never the raw neon value.
    const auto dank_dark = su::app::parse_material_theme(with_dank16(dms_palette()), ThemeMode::dark);
    require(dank_dark.has_value(), "palette with dank16 should parse");
    require(dank_dark->ok != color("#7fff95"), "neon dank16 green must be toned down");
    require(dank_dark->ok.green > dank_dark->ok.red && dank_dark->ok.green > dank_dark->ok.blue,
            "dank16 green must stay green");
    require(dank_dark->warn.red > dank_dark->warn.blue, "dank16 yellow must stay warm");
    require(dank_dark->ok != dark->ok, "dank16 should change the status hue");
    require_readable_roles(*dank_dark, "dank16 dark");
    const auto dank_light = su::app::parse_material_theme(with_dank16(dms_palette()), ThemeMode::light);
    require(dank_light.has_value(), "light palette with dank16 should parse");
    require_readable_roles(*dank_light, "dank16 light");

    // Without dank16 the base hues are harmonised toward the primary.
    const auto blue = su::app::parse_material_theme(dms_palette("#6677ff"), ThemeMode::dark);
    const auto red = su::app::parse_material_theme(dms_palette("#ff5566"), ThemeMode::dark);
    require(blue.has_value() && red.has_value(), "recoloured palettes should parse");
    require(blue->ok != red->ok, "harmonised status colours follow the palette hue");
    require(blue->ok.green > blue->ok.red, "harmonised green stays greenish");
    require_readable_roles(*blue, "harmonised blue");

    const auto fallback = su::app::parse_material_theme(
        dms_palette("#bbc3ff", false), ThemeMode::dark);
    require(fallback.has_value(), "optional Material roles should have same-level fallbacks");
    require(fallback->group == color("#131318"), "container fallback should use Material surface");

    for (const auto mode : {ThemeMode::dark, ThemeMode::light}) {
        require_readable_roles(su::app::built_in_theme(mode), "built-in");
    }

    require(
        !su::app::parse_material_theme(R"({"colors":{"dark":{}}})", ThemeMode::dark),
        "missing required roles should fail");
    require(
        !su::app::parse_material_theme(std::string(1024 * 1024 + 1, 'x'), ThemeMode::dark),
        "oversized palettes should fail");

    auto low_contrast = dms_palette();
    const auto position = low_contrast.find("\"on_surface\":\"#e4e1e9\"");
    require(position != std::string::npos, "low contrast fixture should contain on_surface");
    low_contrast.replace(position, std::string_view{"\"on_surface\":\"#e4e1e9\""}.size(),
                         "\"on_surface\":\"#131318\"");
    const auto corrected = su::app::parse_material_theme(low_contrast, ThemeMode::dark);
    require(corrected.has_value(), "low contrast palette should be corrected, not rejected");
    require(
        su::app::contrast_ratio(corrected->text, corrected->canvas) >= 4.5,
        "corrected primary text should meet contrast requirements");

    const auto matugen = su::app::parse_material_theme(matugen_palette(), ThemeMode::dark);
    require(matugen.has_value(), "Matugen 4 palette structure should parse");
    require(matugen->accent == dark->accent, "DMS and Matugen mappings should agree");
}

void test_ui_preferences(const std::filesystem::path& root) {
    const auto path = root / "ui.json";
    write_file(path, R"({"language":"zh-CN"})");
    const auto legacy = su::app::load_ui_preferences(path);
    require(legacy.has_value(), "legacy language-only preferences should load");
    require(legacy->language == "zh-CN", "legacy language should be retained");
    require(legacy->theme == ThemePreference::system, "legacy theme should default to system");
    require(
        legacy->window_controls == WindowControlsPreference::automatic,
        "legacy window controls should default to automatic");
    require(
        !legacy->desktop_auth_test_passed,
        "legacy authentication test state should default to incomplete");

    auto updated = *legacy;
    updated.theme = ThemePreference::dark;
    updated.window_controls = WindowControlsPreference::hidden;
    updated.desktop_auth_test_passed = true;
    require(
        su::app::save_ui_preferences(path, updated).has_value(),
        "complete UI preferences should save");
    const auto reloaded = su::app::load_ui_preferences(path);
    require(reloaded == updated, "saving appearance settings must retain language");

    write_file(path, R"({"theme_mode":7})");
    require(!su::app::load_ui_preferences(path), "non-string theme mode should fail");
    write_file(path, R"({"window_controls":"sometimes"})");
    require(!su::app::load_ui_preferences(path), "unknown window controls mode should fail");
    write_file(path, R"({"language":false})");
    require(!su::app::load_ui_preferences(path), "non-string language should fail");
    write_file(path, R"({"desktop_auth_test_passed":"yes"})");
    require(
        !su::app::load_ui_preferences(path),
        "non-boolean authentication test state should fail");
}

void test_window_controls_policy() {
    const auto niri = su::app::DesktopEnvironment{
        .current_desktop = "niri", .session_desktop = {}, .desktop_session = {}};
    const auto dwm = su::app::DesktopEnvironment{
        .current_desktop = {}, .session_desktop = {}, .desktop_session = "dwm"};
    const auto gnome = su::app::DesktopEnvironment{
        .current_desktop = "GNOME", .session_desktop = {}, .desktop_session = {}};
    const auto kde = su::app::DesktopEnvironment{
        .current_desktop = "KDE:Plasma", .session_desktop = {}, .desktop_session = {}};
    const auto unknown = su::app::DesktopEnvironment{};

    require(su::app::is_standalone_window_manager(niri), "Niri should be detected as a standalone WM");
    require(su::app::is_standalone_window_manager(dwm), "dwm should be detected as a standalone WM");
    require(
        !su::app::window_controls_visible(WindowControlsPreference::automatic, niri),
        "automatic controls should be hidden under Niri");
    require(
        !su::app::window_controls_visible(WindowControlsPreference::automatic, dwm),
        "automatic controls should be hidden under dwm");
    require(
        su::app::window_controls_visible(WindowControlsPreference::automatic, gnome),
        "automatic controls should be visible under GNOME");
    require(
        su::app::window_controls_visible(WindowControlsPreference::automatic, kde),
        "automatic controls should be visible under KDE Plasma");
    require(
        su::app::window_controls_visible(WindowControlsPreference::automatic, unknown),
        "unknown desktops should preserve native window controls");
    require(
        su::app::window_controls_visible(WindowControlsPreference::visible, niri),
        "show should override the standalone WM default");
    require(
        !su::app::window_controls_visible(WindowControlsPreference::hidden, gnome),
        "hide should override the desktop environment default");
}

void test_explicit_theme_mode(const std::filesystem::path& root) {
    const auto paths = su::app::ThemePaths{
        .dms_palette = root / "dms-colors.json",
        .dms_session = root / "session.json",
    };
    write_file(paths.dms_palette, dms_palette());
    auto mode_calls = 0;
    const auto runner = [&mode_calls](const std::vector<std::string>& arguments, auto, auto)
        -> std::expected<std::string, std::string> {
        if (arguments == std::vector<std::string>{"dms", "ipc", "call", "theme", "getMode"}) {
            ++mode_calls;
            return "dark";
        }
        return std::unexpected("unavailable");
    };

    const auto system = su::app::load_desktop_theme(paths, runner, ThemePreference::system);
    require(system.snapshot.theme.mode == ThemeMode::dark, "system mode should use DMS mode");
    require(mode_calls == 1, "system mode should query the desktop mode");

    const auto light = su::app::load_desktop_theme(paths, runner, ThemePreference::light);
    require(light.snapshot.theme.mode == ThemeMode::light, "explicit light should select the light scheme");
    require(light.snapshot.theme.accent == color("#525a92"), "explicit light should use light colors");
    require(mode_calls == 1, "explicit mode should not query the desktop mode");
}

void test_gnome_wallpaper_fallback(const std::filesystem::path& root) {
    const auto wallpaper = root / "GNOME wallpaper.png";
    write_file(wallpaper, "fixture");
    const auto paths = su::app::ThemePaths{
        .dms_palette = root / "missing-palette.json",
        .dms_session = root / "missing-session.json",
        .desktop = su::app::DesktopEnvironment{.current_desktop = "GNOME"},
    };
    auto encoded = wallpaper.string();
    for (auto position = encoded.find(' '); position != std::string::npos;
         position = encoded.find(' ', position + 3)) {
        encoded.replace(position, 1, "%20");
    }
    auto matugen_path = std::string{};
    const auto runner = [&](const std::vector<std::string>& arguments, auto, auto)
        -> std::expected<std::string, std::string> {
        if (arguments.size() == 4 && arguments[0] == "gsettings"
            && arguments[3] == "picture-uri-dark") {
            return std::format("'file://{}'", encoded);
        }
        if (!arguments.empty() && arguments[0] == "matugen") {
            matugen_path = arguments[2];
            return matugen_palette();
        }
        return std::unexpected("unavailable");
    };
    const auto loaded = su::app::load_desktop_theme(paths, runner, ThemePreference::dark);
    require(loaded.snapshot.source == ThemeSource::matugen, "GNOME wallpaper should feed Matugen");
    require(matugen_path == wallpaper.string(), "GNOME file URI should be percent-decoded safely");
}

void test_source_priority_and_fallback(const std::filesystem::path& root) {
    const auto paths = su::app::ThemePaths{
        .dms_palette = root / "cache" / "dms-colors.json",
        .dms_session = root / "state" / "session.json",
    };
    write_file(paths.dms_palette, dms_palette());
    write_file(paths.dms_session, R"({"isLightMode":true,"wallpaperPath":""})");

    const auto dms_runner = [](const std::vector<std::string>& arguments, auto, auto)
        -> std::expected<std::string, std::string> {
        if (arguments == std::vector<std::string>{"dms", "ipc", "call", "theme", "getMode"}) {
            return "dark\n";
        }
        return std::unexpected("unexpected command");
    };
    const auto loaded = su::app::load_desktop_theme(paths, dms_runner);
    require(loaded.snapshot.source == ThemeSource::dms_cache, "DMS cache should have priority");
    require(loaded.snapshot.theme.mode == ThemeMode::dark, "DMS IPC mode should beat session mode");

    write_file(paths.dms_palette, "{broken");
    const auto rejected = su::app::load_desktop_theme(paths, dms_runner);
    require(
        std::ranges::contains(rejected.rejected_sources, ThemeSource::dms_cache),
        "an invalid DMS cache should be reported as a rejected source");

    std::filesystem::remove(paths.dms_palette);
    const auto wallpaper = root / "wallpaper.png";
    write_file(wallpaper, "fixture");
    auto saw_safe_matugen_arguments = false;
    const auto matugen_json = matugen_palette("#aabbff");
    const auto matugen_runner = [&](const std::vector<std::string>& arguments, auto, auto)
        -> std::expected<std::string, std::string> {
        if (arguments.size() >= 5 && arguments[0] == "dms" && arguments[4] == "getMode") {
            return "dark";
        }
        if (arguments.size() >= 5 && arguments[0] == "dms" && arguments[4] == "get") {
            return std::format("\"{}\"", wallpaper.string());
        }
        if (!arguments.empty() && arguments[0] == "matugen") {
            saw_safe_matugen_arguments = std::ranges::find(
                                             arguments, std::string("--dry-run"))
                    != arguments.end()
                && std::ranges::find(arguments, std::string("--source-color-index"))
                    != arguments.end()
                && arguments[1] == "image"
                && arguments[2] == wallpaper.string();
            return matugen_json;
        }
        return std::unexpected("unexpected command");
    };
    const auto generated = su::app::load_desktop_theme(paths, matugen_runner);
    require(generated.snapshot.source == ThemeSource::matugen, "Matugen should back up DMS cache");
    require(saw_safe_matugen_arguments, "Matugen should receive a fixed argument vector");

    const auto missing = su::app::ThemePaths{
        .dms_palette = root / "missing-cache.json",
        .dms_session = root / "missing-session.json",
    };
    const auto unavailable = [](const auto&, auto, auto)
        -> std::expected<std::string, std::string> {
        return std::unexpected("unavailable");
    };
    const auto built_in = su::app::load_desktop_theme(missing, unavailable);
    require(built_in.snapshot.source == ThemeSource::built_in, "missing sources should use built-in theme");
}

void test_live_refresh(const std::filesystem::path& root) {
    const auto paths = su::app::ThemePaths{
        .dms_palette = root / "live-cache" / "dms-colors.json",
        .dms_session = root / "live-state" / "session.json",
    };
    write_file(paths.dms_palette, dms_palette("#bbc3ff"));
    write_file(paths.dms_session, R"({"isLightMode":false,"wallpaperPath":""})");
    const auto runner = [](const std::vector<std::string>& arguments, auto, auto)
        -> std::expected<std::string, std::string> {
        if (arguments.size() >= 5 && arguments[0] == "dms" && arguments[4] == "getMode") {
            return "dark";
        }
        return std::unexpected("unavailable");
    };
    const auto initial = su::app::load_desktop_theme(paths, runner);
    require(initial.snapshot.source == ThemeSource::dms_cache, "live refresh needs an initial DMS theme");

    auto mutex = std::mutex{};
    auto changed = std::condition_variable{};
    auto update = std::optional<su::app::ThemeLoadResult>{};
    auto callback_count = std::atomic<int>{0};
    const auto monitor = su::app::ThemeMonitor(
        paths,
        runner,
        initial.snapshot,
        ThemePreference::system,
        [&](su::app::ThemeLoadResult loaded) {
            {
                const auto lock = std::lock_guard(mutex);
                update = std::move(loaded);
                callback_count.fetch_add(1, std::memory_order_release);
            }
            changed.notify_one();
        });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!monitor.available() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    require(monitor.available(), "inotify monitor should be available");

    atomic_replace(paths.dms_palette, "{broken");
    std::this_thread::sleep_for(std::chrono::milliseconds{350});
    require(
        callback_count.load(std::memory_order_acquire) == 0,
        "a damaged replacement must preserve the current theme");

    atomic_replace(paths.dms_palette, dms_palette("#aabbff"));
    {
        auto lock = std::unique_lock(mutex);
        require(
            changed.wait_for(lock, std::chrono::seconds{2}, [&] { return update.has_value(); }),
            "valid atomic replacement should refresh the theme");
        require(update->snapshot.source == ThemeSource::dms_cache, "refresh should stay on DMS");
        require(update->snapshot.theme.accent == color("#aabbff"), "refresh should apply new colors");
    }
}

void test_runtime_theme_preference(const std::filesystem::path& root) {
    const auto paths = su::app::ThemePaths{
        .dms_palette = root / "live-cache" / "dms-colors.json",
        .dms_session = root / "live-state" / "session.json",
    };
    write_file(paths.dms_palette, dms_palette());
    write_file(paths.dms_session, R"({"isLightMode":false})");
    const auto runner = [](const std::vector<std::string>& arguments, auto, auto)
        -> std::expected<std::string, std::string> {
        if (arguments.size() >= 5 && arguments[0] == "dms" && arguments[4] == "getMode") {
            return "dark";
        }
        return std::unexpected("unavailable");
    };
    const auto initial = su::app::load_desktop_theme(paths, runner);
    auto mutex = std::mutex{};
    auto changed = std::condition_variable{};
    auto mode = std::optional<ThemeMode>{};
    auto monitor = su::app::ThemeMonitor(
        paths,
        runner,
        initial.snapshot,
        ThemePreference::system,
        [&](su::app::ThemeLoadResult loaded) {
            {
                const auto lock = std::lock_guard(mutex);
                mode = loaded.snapshot.theme.mode;
            }
            changed.notify_one();
        });
    monitor.set_theme_preference(ThemePreference::light);
    auto lock = std::unique_lock(mutex);
    require(
        changed.wait_for(lock, std::chrono::seconds{2}, [&] { return mode.has_value(); }),
        "runtime theme preference should trigger an immediate reload");
    require(mode == ThemeMode::light, "runtime preference should apply the selected scheme");
}

}  // namespace

int main() {
    const auto root = std::filesystem::current_path() / "build" / "test-data" / "theme-unit";
    auto error = std::error_code{};
    std::filesystem::remove_all(root, error);
    const auto tree = TestTree{.root = root};

    try {
        test_color_and_mode_parsing();
        test_palette_mapping_and_validation();
        test_no_role_keeps_builtin_fallback();
        test_ui_preferences(root / "preferences");
        test_window_controls_policy();
        test_explicit_theme_mode(root / "explicit-mode");
        test_gnome_wallpaper_fallback(root / "gnome");
        test_source_priority_and_fallback(root / "sources");
        test_live_refresh(root / "monitor");
        test_runtime_theme_preference(root / "runtime-preference");
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "theme test failed: " << failure.what() << '\n';
        return 1;
    }
}
