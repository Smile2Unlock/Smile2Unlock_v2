module;

#include <nlohmann/json.hpp>

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

export module su.app.theme;

import std;
import su.app.preferences;

export namespace su::app {

enum class ThemeMode {
    light,
    dark,
};

enum class ThemeSource {
    built_in,
    matugen,
    dms_cache,
};

struct ThemeColor {
    std::uint8_t alpha = 255;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    auto operator<=>(const ThemeColor&) const = default;
};

struct AppTheme {
    ThemeMode mode = ThemeMode::light;
    ThemeColor canvas;
    ThemeColor surface;
    ThemeColor surface_subtle;
    ThemeColor surface_selected;
    ThemeColor border;
    ThemeColor divider;
    ThemeColor text_primary;
    ThemeColor text_secondary;
    ThemeColor text_tertiary;
    ThemeColor primary;
    ThemeColor primary_hover;
    ThemeColor primary_pressed;
    ThemeColor on_primary;
    ThemeColor success_surface;
    ThemeColor success_text;
    ThemeColor warning_surface;
    ThemeColor warning_text;
    ThemeColor danger_surface;
    ThemeColor danger_hover;
    ThemeColor danger_text;
    ThemeColor disabled_surface;
    ThemeColor disabled_text;
    ThemeColor preview_surface;
    ThemeColor preview_overlay;
    ThemeColor preview_text;
    ThemeColor face_indicator;

    auto operator<=>(const AppTheme&) const = default;
};

struct ThemeSnapshot {
    AppTheme theme;
    ThemeSource source = ThemeSource::built_in;

    auto operator<=>(const ThemeSnapshot&) const = default;
};

struct ThemePaths {
    std::filesystem::path dms_palette;
    std::filesystem::path dms_session;
    DesktopEnvironment desktop{};
};

struct ThemeLoadResult {
    ThemeSnapshot snapshot;
    std::vector<std::string> diagnostics;
};

using ThemeCommandRunner = std::function<std::expected<std::string, std::string>(
    const std::vector<std::string>&,
    std::chrono::milliseconds,
    std::size_t)>;

std::expected<ThemeColor, std::string> parse_theme_color(std::string_view text);
std::expected<ThemeMode, std::string> parse_dms_mode(std::string_view text);
std::expected<ThemeMode, std::string> parse_dms_session_mode(std::string_view text);
std::expected<AppTheme, std::string> parse_material_theme(
    std::string_view text,
    ThemeMode mode);
[[nodiscard]] AppTheme built_in_theme(ThemeMode mode);
[[nodiscard]] double contrast_ratio(ThemeColor foreground, ThemeColor background);
[[nodiscard]] ThemePaths default_theme_paths();
[[nodiscard]] ThemeCommandRunner system_theme_command_runner();
[[nodiscard]] ThemeLoadResult load_desktop_theme(
    const ThemePaths& paths,
    const ThemeCommandRunner& run_command,
    ThemePreference preference = ThemePreference::system);

class ThemeMonitor {
public:
    using UpdateCallback = std::move_only_function<void(ThemeLoadResult)>;

    ThemeMonitor(
        ThemePaths paths,
        ThemeCommandRunner run_command,
        ThemeSnapshot initial,
        ThemePreference preference,
        UpdateCallback callback);
    ~ThemeMonitor();
    ThemeMonitor(const ThemeMonitor&) = delete;
    ThemeMonitor& operator=(const ThemeMonitor&) = delete;
    ThemeMonitor(ThemeMonitor&&) = delete;
    ThemeMonitor& operator=(ThemeMonitor&&) = delete;

    [[nodiscard]] bool available() const;
    void set_theme_preference(ThemePreference preference);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace su::app

namespace su::app {

namespace {

constexpr auto max_theme_input_size = std::size_t{1024 * 1024};
constexpr auto dms_ipc_timeout = std::chrono::milliseconds{400};
constexpr auto matugen_timeout = std::chrono::seconds{4};

constexpr ThemeColor rgb(std::uint32_t value) {
    return ThemeColor{
        .alpha = 255,
        .red = static_cast<std::uint8_t>((value >> 16) & 0xff),
        .green = static_cast<std::uint8_t>((value >> 8) & 0xff),
        .blue = static_cast<std::uint8_t>(value & 0xff),
    };
}

constexpr ThemeColor argb(std::uint32_t value) {
    return ThemeColor{
        .alpha = static_cast<std::uint8_t>((value >> 24) & 0xff),
        .red = static_cast<std::uint8_t>((value >> 16) & 0xff),
        .green = static_cast<std::uint8_t>((value >> 8) & 0xff),
        .blue = static_cast<std::uint8_t>(value & 0xff),
    };
}

constexpr std::string_view mode_name(ThemeMode mode) {
    return mode == ThemeMode::dark ? "dark" : "light";
}

ThemeColor blend(ThemeColor first, ThemeColor second, double second_weight) {
    const auto channel = [second_weight](std::uint8_t left, std::uint8_t right) {
        return static_cast<std::uint8_t>(std::clamp(
            std::lround(
                static_cast<double>(left) * (1.0 - second_weight)
                + static_cast<double>(right) * second_weight),
            0L,
            255L));
    };
    return ThemeColor{
        .alpha = channel(first.alpha, second.alpha),
        .red = channel(first.red, second.red),
        .green = channel(first.green, second.green),
        .blue = channel(first.blue, second.blue),
    };
}

double linear_channel(std::uint8_t value) {
    const auto channel = static_cast<double>(value) / 255.0;
    return channel <= 0.04045
        ? channel / 12.92
        : std::pow((channel + 0.055) / 1.055, 2.4);
}

double luminance(ThemeColor color) {
    return 0.2126 * linear_channel(color.red)
        + 0.7152 * linear_channel(color.green)
        + 0.0722 * linear_channel(color.blue);
}

ThemeColor composite_over(ThemeColor foreground, ThemeColor background) {
    const auto opacity = static_cast<double>(foreground.alpha) / 255.0;
    const auto channel = [opacity](std::uint8_t front, std::uint8_t back) {
        return static_cast<std::uint8_t>(std::clamp(
            std::lround(
                static_cast<double>(front) * opacity
                + static_cast<double>(back) * (1.0 - opacity)),
            0L,
            255L));
    };
    return ThemeColor{
        .alpha = 255,
        .red = channel(foreground.red, background.red),
        .green = channel(foreground.green, background.green),
        .blue = channel(foreground.blue, background.blue),
    };
}

ThemeColor safe_foreground(
    ThemeColor candidate,
    ThemeColor background,
    double minimum_ratio) {
    if (contrast_ratio(candidate, background) >= minimum_ratio) {
        return candidate;
    }
    const auto dark = rgb(0x101114);
    const auto light = rgb(0xf8f9ff);
    return contrast_ratio(dark, background) >= contrast_ratio(light, background) ? dark : light;
}

std::expected<std::string, std::string> read_file_limited(
    const std::filesystem::path& path,
    std::size_t limit = max_theme_input_size) {
    auto error = std::error_code{};
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected(std::format("cannot inspect {}: {}", path.string(), error.message()));
    }
    if (size > limit) {
        return std::unexpected(std::format("{} exceeds the theme input limit", path.string()));
    }
    auto stream = std::ifstream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(std::format("cannot open {}", path.string()));
    }
    auto text = std::string(std::istreambuf_iterator<char>{stream}, {});
    if (stream.bad() || text.size() > limit) {
        return std::unexpected(std::format("failed to read {} safely", path.string()));
    }
    return text;
}

std::optional<std::uint8_t> parse_hex_byte(std::string_view text) {
    auto value = unsigned{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (error != std::errc{} || end != text.data() + text.size() || value > 0xff) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

using Json = nlohmann::json;

std::expected<std::optional<ThemeColor>, std::string> material_role(
    const Json& document,
    ThemeMode mode,
    std::string_view role) {
    if (!document.contains("colors") || !document["colors"].is_object()) {
        return std::unexpected("Material palette is missing the colors object");
    }
    const auto& colors = document["colors"];
    const auto selected_mode = std::string(mode_name(mode));
    const Json* value = nullptr;

    if (colors.contains(selected_mode) && colors[selected_mode].is_object()
        && colors[selected_mode].contains(role)) {
        value = &colors[selected_mode][role];
    } else if (colors.contains(role) && colors[role].is_object()
               && colors[role].contains(selected_mode)) {
        value = &colors[role][selected_mode];
    }
    if (value == nullptr) {
        return std::optional<ThemeColor>{};
    }

    const Json* color_value = value;
    if (value->is_object() && value->contains("color")) {
        color_value = &(*value)["color"];
    }
    if (!color_value->is_string()) {
        return std::unexpected(std::format(
            "Material role '{}' is not a color string", role));
    }
    auto parsed = parse_theme_color(color_value->get_ref<const std::string&>());
    if (!parsed) {
        return std::unexpected(std::format("Material role '{}': {}", role, parsed.error()));
    }
    return std::optional{*parsed};
}

std::expected<ThemeColor, std::string> required_role(
    const Json& document,
    ThemeMode mode,
    std::string_view role) {
    auto value = material_role(document, mode, role);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!*value) {
        return std::unexpected(std::format("Material palette is missing role '{}'", role));
    }
    return **value;
}

std::expected<ThemeColor, std::string> optional_role(
    const Json& document,
    ThemeMode mode,
    std::string_view role,
    ThemeColor fallback) {
    auto value = material_role(document, mode, role);
    if (!value) {
        return std::unexpected(value.error());
    }
    return *value ? **value : fallback;
}

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::optional<ThemeMode> portal_mode(std::string_view output) {
    if (output.find("uint32 1") != std::string_view::npos) {
        return ThemeMode::dark;
    }
    if (output.find("uint32 2") != std::string_view::npos) {
        return ThemeMode::light;
    }
    return std::nullopt;
}

std::expected<std::filesystem::path, std::string> session_wallpaper(
    std::string_view text,
    ThemeMode mode) {
    const auto document = Json::parse(text.begin(), text.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return std::unexpected("DMS session state is not valid JSON");
    }
    const auto per_mode = document.value("perModeWallpaper", false);
    const auto selected_key = mode == ThemeMode::dark ? "wallpaperPathDark" : "wallpaperPathLight";
    if (per_mode && document.contains(selected_key) && document[selected_key].is_string()) {
        const auto selected = document[selected_key].get<std::string>();
        if (!selected.empty()) {
            return std::filesystem::path(selected);
        }
    }
    if (!document.contains("wallpaperPath") || !document["wallpaperPath"].is_string()) {
        return std::unexpected("DMS session state has no wallpaper path");
    }
    const auto wallpaper = document["wallpaperPath"].get<std::string>();
    if (wallpaper.empty()) {
        return std::unexpected("DMS wallpaper path is empty");
    }
    return std::filesystem::path(wallpaper);
}

std::expected<std::filesystem::path, std::string> wallpaper_from_ipc(std::string_view output) {
    const auto stripped = trim(output);
    if (stripped.empty()) {
        return std::unexpected("DMS wallpaper IPC returned an empty path");
    }
    const auto document = Json::parse(stripped, nullptr, false);
    if (document.is_string()) {
        return std::filesystem::path(document.get<std::string>());
    }
    if (document.is_object()) {
        for (const auto* key : {"path", "wallpaper", "wallpaperPath"}) {
            if (document.contains(key) && document[key].is_string()) {
                return std::filesystem::path(document[key].get<std::string>());
            }
        }
    }
    if ((stripped.front() == '\'' && stripped.back() == '\'')
        || (stripped.front() == '"' && stripped.back() == '"')) {
        return std::filesystem::path(stripped.substr(1, stripped.size() - 2));
    }
    return std::filesystem::path(stripped);
}

std::expected<std::filesystem::path, std::string> wallpaper_from_gsettings(
    std::string_view output) {
    auto value = trim(output);
    if (value.size() >= 2
        && ((value.front() == '\'' && value.back() == '\'')
            || (value.front() == '"' && value.back() == '"'))) {
        value = value.substr(1, value.size() - 2);
    }
    if (!value.starts_with("file://")) {
        return std::unexpected("GNOME wallpaper setting is not a local file URI");
    }

    auto encoded_path = std::string_view(value).substr(7);
    if (encoded_path.starts_with("localhost/")) {
        encoded_path.remove_prefix(std::string_view("localhost").size());
    }
    auto decoded = std::string{};
    decoded.reserve(encoded_path.size());
    for (auto index = std::size_t{0}; index < encoded_path.size();) {
        if (encoded_path[index] == '%' && index + 2 < encoded_path.size()) {
            if (const auto byte = parse_hex_byte(encoded_path.substr(index + 1, 2))) {
                decoded.push_back(static_cast<char>(*byte));
                index += 3;
                continue;
            }
        }
        decoded.push_back(encoded_path[index]);
        ++index;
    }
    if (decoded.empty() || decoded.front() != '/') {
        return std::unexpected("GNOME wallpaper URI has no absolute path");
    }
    return std::filesystem::path(decoded);
}

std::optional<ThemeMode> preferred_theme_mode(ThemePreference preference) {
    switch (preference) {
        case ThemePreference::light:
            return ThemeMode::light;
        case ThemePreference::dark:
            return ThemeMode::dark;
        case ThemePreference::system:
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> gnome_wallpaper(
    const ThemePaths& paths,
    const ThemeCommandRunner& run_command,
    ThemeMode mode) {
    if (!uses_gnome_desktop(paths.desktop)) {
        return std::nullopt;
    }
    const auto keys = mode == ThemeMode::dark
        ? std::array<std::string_view, 2>{"picture-uri-dark", "picture-uri"}
        : std::array<std::string_view, 2>{"picture-uri", "picture-uri-dark"};
    for (const auto key : keys) {
        const auto output = run_command(
            {"gsettings", "get", "org.gnome.desktop.background", std::string(key)},
            dms_ipc_timeout,
            64 * 1024);
        if (!output) {
            continue;
        }
        if (const auto parsed = wallpaper_from_gsettings(*output); parsed) {
            return *parsed;
        }
    }
    return std::nullopt;
}

std::string source_name(ThemeSource source) {
    switch (source) {
        case ThemeSource::dms_cache:
            return "DMS cache";
        case ThemeSource::matugen:
            return "Matugen";
        case ThemeSource::built_in:
            return "built-in";
    }
    return "unknown";
}

int source_priority(ThemeSource source) {
    return static_cast<int>(source);
}

#if defined(__linux__)

std::expected<std::string, std::string> run_process(
    const std::vector<std::string>& arguments,
    std::chrono::milliseconds timeout,
    std::size_t output_limit) {
    if (arguments.empty()) {
        return std::unexpected("cannot run an empty command");
    }

    int output_pipe[2] = {-1, -1};
    if (::pipe(output_pipe) != 0) {
        return std::unexpected(std::format("pipe failed: {}", std::strerror(errno)));
    }
    (void)::fcntl(output_pipe[0], F_SETFL, ::fcntl(output_pipe[0], F_GETFL) | O_NONBLOCK);
    (void)::fcntl(output_pipe[0], F_SETFD, FD_CLOEXEC);
    (void)::fcntl(output_pipe[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
    ::posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
    ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    auto argv = std::vector<char*>{};
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    auto child = pid_t{};
    const auto spawn_error = ::posix_spawnp(
        &child,
        arguments.front().c_str(),
        &actions,
        nullptr,
        argv.data(),
        environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(output_pipe[1]);
    if (spawn_error != 0) {
        ::close(output_pipe[0]);
        return std::unexpected(std::format(
            "failed to start {}: {}", arguments.front(), std::strerror(spawn_error)));
    }

    auto output = std::string{};
    auto status = 0;
    auto child_finished = false;
    auto pipe_finished = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline && (!child_finished || !pipe_finished)) {
        pollfd descriptor{.fd = output_pipe[0], .events = POLLIN | POLLHUP, .revents = 0};
        (void)::poll(&descriptor, 1, 40);
        if ((descriptor.revents & (POLLIN | POLLHUP)) != 0) {
            auto buffer = std::array<char, 4096>{};
            while (true) {
                const auto count = ::read(output_pipe[0], buffer.data(), buffer.size());
                if (count > 0) {
                    output.append(buffer.data(), static_cast<std::size_t>(count));
                    if (output.size() > output_limit) {
                        (void)::kill(child, SIGKILL);
                        (void)::waitpid(child, &status, 0);
                        ::close(output_pipe[0]);
                        return std::unexpected(std::format(
                            "{} output exceeded the limit", arguments.front()));
                    }
                    continue;
                }
                if (count == 0) {
                    pipe_finished = true;
                }
                break;
            }
        }
        if (!child_finished) {
            const auto waited = ::waitpid(child, &status, WNOHANG);
            child_finished = waited == child;
        }
    }

    if (!child_finished) {
        (void)::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
        ::close(output_pipe[0]);
        return std::unexpected(std::format("{} timed out", arguments.front()));
    }
    ::close(output_pipe[0]);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected(std::format("{} exited unsuccessfully", arguments.front()));
    }
    return output;
}

#endif

}  // namespace

std::expected<ThemeColor, std::string> parse_theme_color(std::string_view text) {
    if (text.size() != 7 && text.size() != 9) {
        return std::unexpected("expected #RRGGBB or #AARRGGBB");
    }
    if (text.front() != '#') {
        return std::unexpected("color must start with '#'");
    }
    const auto offset = text.size() == 9 ? std::size_t{3} : std::size_t{1};
    const auto alpha = text.size() == 9 ? parse_hex_byte(text.substr(1, 2)) : std::optional{std::uint8_t{255}};
    const auto red = parse_hex_byte(text.substr(offset, 2));
    const auto green = parse_hex_byte(text.substr(offset + 2, 2));
    const auto blue = parse_hex_byte(text.substr(offset + 4, 2));
    if (!alpha || !red || !green || !blue) {
        return std::unexpected("color contains invalid hexadecimal digits");
    }
    return ThemeColor{.alpha = *alpha, .red = *red, .green = *green, .blue = *blue};
}

std::expected<ThemeMode, std::string> parse_dms_mode(std::string_view text) {
    auto normalized = trim(text);
    const auto document = Json::parse(normalized, nullptr, false);
    if (document.is_string()) {
        normalized = document.get<std::string>();
    }
    std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (normalized == "dark") {
        return ThemeMode::dark;
    }
    if (normalized == "light") {
        return ThemeMode::light;
    }
    return std::unexpected("DMS mode must be 'dark' or 'light'");
}

std::expected<ThemeMode, std::string> parse_dms_session_mode(std::string_view text) {
    if (text.size() > max_theme_input_size) {
        return std::unexpected("DMS session state exceeds the input limit");
    }
    const auto document = Json::parse(text.begin(), text.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()
        || !document.contains("isLightMode") || !document["isLightMode"].is_boolean()) {
        return std::unexpected("DMS session state has no boolean isLightMode");
    }
    return document["isLightMode"].get<bool>() ? ThemeMode::light : ThemeMode::dark;
}

AppTheme built_in_theme(ThemeMode mode) {
    if (mode == ThemeMode::dark) {
        return AppTheme{
            .mode = mode,
            .canvas = rgb(0x121614),
            .surface = rgb(0x0b0f0e),
            .surface_subtle = rgb(0x1b2421),
            .surface_selected = rgb(0x173b32),
            .border = rgb(0x3c4945),
            .divider = rgb(0x29332f),
            .text_primary = rgb(0xe1e8e5),
            .text_secondary = rgb(0xbdc9c5),
            .text_tertiary = rgb(0x8f9b97),
            .primary = rgb(0x84d7bc),
            .primary_hover = rgb(0x73c5aa),
            .primary_pressed = rgb(0x5eaa91),
            .on_primary = rgb(0x0a392e),
            .success_surface = rgb(0x123c31),
            .success_text = rgb(0x8fe2c1),
            .warning_surface = rgb(0x463813),
            .warning_text = rgb(0xf2d17a),
            .danger_surface = rgb(0x93000a),
            .danger_hover = rgb(0xaa1b20),
            .danger_text = rgb(0xffdad6),
            .disabled_surface = rgb(0x29332f),
            .disabled_text = rgb(0x8f9b97),
            .preview_surface = rgb(0x101816),
            .preview_overlay = argb(0xcc15201e),
            .preview_text = rgb(0xe8f6f1),
            .face_indicator = rgb(0x4dd3a7),
        };
    }
    return AppTheme{
        .mode = mode,
        .canvas = rgb(0xf3f6f5),
        .surface = rgb(0xffffff),
        .surface_subtle = rgb(0xedf3f1),
        .surface_selected = rgb(0xdcece7),
        .border = rgb(0xd6dfdc),
        .divider = rgb(0xe2e8e6),
        .text_primary = rgb(0x182d27),
        .text_secondary = rgb(0x65746f),
        .text_tertiary = rgb(0x75827e),
        .primary = rgb(0x155447),
        .primary_hover = rgb(0x1c6655),
        .primary_pressed = rgb(0x0e3a30),
        .on_primary = rgb(0xffffff),
        .success_surface = rgb(0xd8eee5),
        .success_text = rgb(0x165c46),
        .warning_surface = rgb(0xf5e7c5),
        .warning_text = rgb(0x775514),
        .danger_surface = rgb(0xf5dcdc),
        .danger_hover = rgb(0xfff1f1),
        .danger_text = rgb(0x983535),
        .disabled_surface = rgb(0xe8edeb),
        .disabled_text = rgb(0x8b9894),
        .preview_surface = rgb(0x101816),
        .preview_overlay = argb(0xcc15201e),
        .preview_text = rgb(0xe8f6f1),
        .face_indicator = rgb(0x4dd3a7),
    };
}

double contrast_ratio(ThemeColor foreground, ThemeColor background) {
    const auto first = luminance(composite_over(foreground, background));
    const auto second = luminance(background);
    const auto lighter = std::max(first, second);
    const auto darker = std::min(first, second);
    return (lighter + 0.05) / (darker + 0.05);
}

std::expected<AppTheme, std::string> parse_material_theme(
    std::string_view text,
    ThemeMode mode) {
    if (text.size() > max_theme_input_size) {
        return std::unexpected("Material palette exceeds the input limit");
    }
    const auto document = Json::parse(text.begin(), text.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return std::unexpected("Material palette is not valid JSON");
    }

    const auto background = required_role(document, mode, "background");
    const auto surface = required_role(document, mode, "surface");
    const auto primary = required_role(document, mode, "primary");
    const auto on_primary = required_role(document, mode, "on_primary");
    const auto primary_container = required_role(document, mode, "primary_container");
    const auto on_surface = required_role(document, mode, "on_surface");
    const auto on_surface_variant = required_role(document, mode, "on_surface_variant");
    const auto outline = required_role(document, mode, "outline");
    const auto error = required_role(document, mode, "error");
    const auto error_container = required_role(document, mode, "error_container");
    const auto on_error_container = required_role(document, mode, "on_error_container");
    for (const auto* result : {
             &background,
             &surface,
             &primary,
             &on_primary,
             &primary_container,
             &on_surface,
             &on_surface_variant,
             &outline,
             &error,
             &error_container,
             &on_error_container}) {
        if (!*result) {
            return std::unexpected(result->error());
        }
    }

    const auto surface_lowest = optional_role(
        document, mode, "surface_container_lowest", *surface);
    const auto surface_container = optional_role(
        document, mode, "surface_container", *surface);
    if (!surface_lowest) {
        return std::unexpected(surface_lowest.error());
    }
    if (!surface_container) {
        return std::unexpected(surface_container.error());
    }
    const auto surface_high = optional_role(
        document, mode, "surface_container_high", *surface_container);
    const auto outline_variant = optional_role(document, mode, "outline_variant", *outline);
    if (!surface_high) {
        return std::unexpected(surface_high.error());
    }
    if (!outline_variant) {
        return std::unexpected(outline_variant.error());
    }

    const auto fallback = built_in_theme(mode);
    auto theme = fallback;
    theme.canvas = *background;
    theme.surface = *surface_lowest;
    theme.surface_subtle = *surface_container;
    theme.surface_selected = *primary_container;
    theme.border = *outline_variant;
    theme.divider = blend(*outline_variant, *background, 0.45);
    theme.text_primary = safe_foreground(*on_surface, *background, 4.5);
    theme.text_secondary = safe_foreground(*on_surface_variant, *background, 4.5);
    theme.text_tertiary = safe_foreground(*outline, *background, 3.0);
    theme.primary = *primary;
    theme.primary_hover = blend(*primary, *on_primary, 0.08);
    theme.primary_pressed = blend(*primary, *on_primary, 0.16);
    theme.on_primary = safe_foreground(*on_primary, *primary, 4.5);
    theme.danger_surface = *error_container;
    theme.danger_hover = blend(*error_container, *error, 0.12);
    theme.danger_text = safe_foreground(*on_error_container, *error_container, 4.5);
    theme.disabled_surface = *surface_high;
    theme.disabled_text = safe_foreground(*outline, *surface_high, 3.0);

    // Status colors keep their authentication meaning instead of following the accent hue.
    theme.success_surface = fallback.success_surface;
    theme.success_text = fallback.success_text;
    theme.warning_surface = fallback.warning_surface;
    theme.warning_text = fallback.warning_text;
    theme.face_indicator = fallback.face_indicator;
    return theme;
}

ThemePaths default_theme_paths() {
    const auto* home_value = std::getenv("HOME");
    const auto home = home_value != nullptr && home_value[0] != '\0'
        ? std::filesystem::path(home_value)
        : std::filesystem::path{};
    const auto* cache_value = std::getenv("XDG_CACHE_HOME");
    const auto cache = cache_value != nullptr && cache_value[0] != '\0'
        ? std::filesystem::path(cache_value)
        : home / ".cache";
    const auto* state_value = std::getenv("XDG_STATE_HOME");
    const auto state = state_value != nullptr && state_value[0] != '\0'
        ? std::filesystem::path(state_value)
        : home / ".local" / "state";
    return ThemePaths{
        .dms_palette = cache / "DankMaterialShell" / "dms-colors.json",
        .dms_session = state / "DankMaterialShell" / "session.json",
        .desktop = current_desktop_environment(),
    };
}

ThemeCommandRunner system_theme_command_runner() {
#if defined(__linux__)
    return [](const auto& arguments, auto timeout, auto output_limit) {
        return run_process(arguments, timeout, output_limit);
    };
#else
    return [](const auto&, auto, auto) -> std::expected<std::string, std::string> {
        return std::unexpected("desktop theme commands are unavailable on this platform");
    };
#endif
}

ThemeLoadResult load_desktop_theme(
    const ThemePaths& paths,
    const ThemeCommandRunner& run_command,
    ThemePreference preference) {
    auto diagnostics = std::vector<std::string>{};
    auto session_text = read_file_limited(paths.dms_session);

    auto mode = preferred_theme_mode(preference);
    if (!mode) {
        const auto ipc = run_command(
            {"dms", "ipc", "call", "theme", "getMode"},
            dms_ipc_timeout,
            64 * 1024);
        if (ipc) {
            if (const auto parsed = parse_dms_mode(*ipc); parsed) {
                mode = *parsed;
            } else {
                diagnostics.push_back(std::format("DMS mode IPC: {}", parsed.error()));
            }
        }
    }
    if (!mode && session_text) {
        if (const auto parsed = parse_dms_session_mode(*session_text); parsed) {
            mode = *parsed;
        } else {
            diagnostics.push_back(std::format("DMS session mode: {}", parsed.error()));
        }
    }
    if (!mode) {
        if (const auto portal = run_command(
                {"gdbus", "call", "--session", "--dest", "org.freedesktop.portal.Desktop",
                 "--object-path", "/org/freedesktop/portal/desktop", "--method",
                 "org.freedesktop.portal.Settings.Read", "org.freedesktop.appearance",
                 "color-scheme"},
                dms_ipc_timeout,
                64 * 1024);
            portal) {
            mode = portal_mode(*portal);
        }
    }
    const auto selected_mode = mode.value_or(ThemeMode::light);

    if (const auto palette = read_file_limited(paths.dms_palette); palette) {
        if (auto parsed = parse_material_theme(*palette, selected_mode); parsed) {
            return ThemeLoadResult{
                .snapshot = ThemeSnapshot{.theme = std::move(*parsed), .source = ThemeSource::dms_cache},
                .diagnostics = std::move(diagnostics),
            };
        } else {
            diagnostics.push_back(std::format(
                "DMS palette {}: {}", paths.dms_palette.string(), parsed.error()));
        }
    }

    auto wallpaper = std::optional<std::filesystem::path>{};
    if (const auto ipc = run_command(
            {"dms", "ipc", "call", "wallpaper", "get"},
            dms_ipc_timeout,
            64 * 1024);
        ipc) {
        if (const auto parsed = wallpaper_from_ipc(*ipc); parsed) {
            wallpaper = *parsed;
        }
    }
    if (!wallpaper && session_text) {
        if (const auto parsed = session_wallpaper(*session_text, selected_mode); parsed) {
            wallpaper = *parsed;
        }
    }
    if (!wallpaper) {
        wallpaper = gnome_wallpaper(paths, run_command, selected_mode);
    }
    auto file_error = std::error_code{};
    if (wallpaper && std::filesystem::is_regular_file(*wallpaper, file_error)) {
        const auto generated = run_command(
            {"matugen", "image", wallpaper->string(), "--dry-run", "-j", "hex", "-m",
             std::string(mode_name(selected_mode)), "-t", "scheme-tonal-spot", "--contrast", "0",
             "--source-color-index", "0"},
            matugen_timeout,
            max_theme_input_size);
        if (generated) {
            if (auto parsed = parse_material_theme(*generated, selected_mode); parsed) {
                return ThemeLoadResult{
                    .snapshot = ThemeSnapshot{.theme = std::move(*parsed), .source = ThemeSource::matugen},
                    .diagnostics = std::move(diagnostics),
                };
            } else {
                diagnostics.push_back(std::format("Matugen palette: {}", parsed.error()));
            }
        }
    }

    return ThemeLoadResult{
        .snapshot = ThemeSnapshot{
            .theme = built_in_theme(selected_mode),
            .source = ThemeSource::built_in,
        },
        .diagnostics = std::move(diagnostics),
    };
}

#if defined(__linux__)

class ThemeMonitor::Impl {
public:
    Impl(
        ThemePaths paths,
        ThemeCommandRunner run_command,
        ThemeSnapshot initial,
        ThemePreference preference,
        UpdateCallback callback)
        : paths_(std::move(paths)),
          run_command_(std::move(run_command)),
          current_(std::move(initial)),
          preference_(preference),
          callback_(std::move(callback)),
          wake_descriptor_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)),
          thread_([this](std::stop_token stop) { run(stop); }) {}

    ~Impl() {
        thread_.request_stop();
        wake();
    }

    [[nodiscard]] bool available() const {
        return available_.load(std::memory_order_acquire);
    }

    void set_theme_preference(ThemePreference preference) {
        preference_.store(preference, std::memory_order_release);
        reload_requested_.store(true, std::memory_order_release);
        wake();
    }

private:
    void wake() const {
        if (wake_descriptor_ < 0) {
            return;
        }
        constexpr auto value = std::uint64_t{1};
        (void)::write(wake_descriptor_, &value, sizeof(value));
    }

    bool relevant_event(const inotify_event& event) const {
        if (event.len == 0) {
            return false;
        }
        const auto name = std::string_view(event.name);
        return name == paths_.dms_palette.filename().string()
            || name == paths_.dms_session.filename().string();
    }

    void reload() {
        auto loaded = load_desktop_theme(
            paths_, run_command_, preference_.load(std::memory_order_acquire));
        if (source_priority(loaded.snapshot.source) < source_priority(current_.source)) {
            return;
        }
        if (loaded.snapshot == current_) {
            return;
        }
        std::println(stderr, "[theme] switched to {}", source_name(loaded.snapshot.source));
        current_ = loaded.snapshot;
        if (callback_) {
            callback_(std::move(loaded));
        }
    }

    void run(std::stop_token stop) {
        const auto descriptor = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (descriptor < 0 && wake_descriptor_ < 0) {
            return;
        }
        auto directories = std::vector{
            paths_.dms_palette.parent_path(),
            paths_.dms_session.parent_path(),
        };
        std::ranges::sort(directories);
        directories.erase(std::unique(directories.begin(), directories.end()), directories.end());

        auto watches = 0;
        constexpr auto mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_ATTRIB;
        if (descriptor >= 0) {
            for (const auto& directory : directories) {
                if (::inotify_add_watch(descriptor, directory.c_str(), mask) >= 0) {
                    ++watches;
                }
            }
        }
        available_.store(watches > 0, std::memory_order_release);
        auto pending = false;
        auto reload_at = std::chrono::steady_clock::time_point{};
        auto buffer = std::array<char, 16 * 1024>{};
        while (!stop.stop_requested()) {
            if (reload_requested_.exchange(false, std::memory_order_acq_rel)) {
                reload();
            }
            auto timeout = wake_descriptor_ >= 0 ? -1 : 250;
            if (pending) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    reload_at - std::chrono::steady_clock::now());
                timeout = static_cast<int>(std::clamp(remaining.count(), 0L, 250L));
            }
            auto poll_descriptors = std::array<pollfd, 2>{};
            auto descriptor_count = nfds_t{0};
            if (wake_descriptor_ >= 0) {
                poll_descriptors[descriptor_count++] = pollfd{
                    .fd = wake_descriptor_, .events = POLLIN, .revents = 0};
            }
            const auto inotify_index = descriptor_count;
            if (descriptor >= 0 && watches > 0) {
                poll_descriptors[descriptor_count++] = pollfd{
                    .fd = descriptor, .events = POLLIN, .revents = 0};
            }
            const auto polled = ::poll(poll_descriptors.data(), descriptor_count, timeout);
            if (wake_descriptor_ >= 0 && polled > 0
                && (poll_descriptors[0].revents & POLLIN) != 0) {
                auto value = std::uint64_t{};
                while (::read(wake_descriptor_, &value, sizeof(value)) > 0) {}
            }
            if (descriptor >= 0 && watches > 0 && polled > 0
                && (poll_descriptors[inotify_index].revents & POLLIN) != 0) {
                const auto count = ::read(descriptor, buffer.data(), buffer.size());
                auto offset = std::size_t{0};
                while (count > 0 && offset + sizeof(inotify_event) <= static_cast<std::size_t>(count)) {
                    const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                    if (relevant_event(*event)) {
                        pending = true;
                        reload_at = std::chrono::steady_clock::now() + std::chrono::milliseconds{160};
                    }
                    offset += sizeof(inotify_event) + event->len;
                }
            }
            if (pending && std::chrono::steady_clock::now() >= reload_at) {
                pending = false;
                reload();
            }
        }
        available_.store(false, std::memory_order_release);
        if (descriptor >= 0) {
            ::close(descriptor);
        }
        if (wake_descriptor_ >= 0) {
            ::close(wake_descriptor_);
            wake_descriptor_ = -1;
        }
    }

    ThemePaths paths_;
    ThemeCommandRunner run_command_;
    ThemeSnapshot current_;
    std::atomic<ThemePreference> preference_;
    UpdateCallback callback_;
    std::atomic<bool> reload_requested_{false};
    std::atomic<bool> available_{false};
    int wake_descriptor_ = -1;
    std::jthread thread_;
};

#else

class ThemeMonitor::Impl {
public:
    Impl(
        ThemePaths paths,
        ThemeCommandRunner run_command,
        ThemeSnapshot initial,
        ThemePreference preference,
        UpdateCallback callback)
        : paths_(std::move(paths)),
          run_command_(std::move(run_command)),
          current_(std::move(initial)),
          preference_(preference),
          callback_(std::move(callback)),
          thread_([this](std::stop_token stop) { run(stop); }) {}

    ~Impl() {
        thread_.request_stop();
        changed_.notify_all();
    }

    [[nodiscard]] bool available() const { return false; }

    void set_theme_preference(ThemePreference preference) {
        preference_.store(preference, std::memory_order_release);
        reload_requested_.store(true, std::memory_order_release);
        changed_.notify_one();
    }

private:
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            auto lock = std::unique_lock(mutex_);
            changed_.wait_for(lock, std::chrono::milliseconds{250}, [this, &stop] {
                return stop.stop_requested()
                    || reload_requested_.load(std::memory_order_acquire);
            });
            if (stop.stop_requested()) {
                break;
            }
            if (!reload_requested_.exchange(false, std::memory_order_acq_rel)) {
                continue;
            }
            lock.unlock();
            auto loaded = load_desktop_theme(
                paths_, run_command_, preference_.load(std::memory_order_acquire));
            if (loaded.snapshot == current_) {
                continue;
            }
            current_ = loaded.snapshot;
            if (callback_) {
                callback_(std::move(loaded));
            }
        }
    }

    ThemePaths paths_;
    ThemeCommandRunner run_command_;
    ThemeSnapshot current_;
    std::atomic<ThemePreference> preference_;
    UpdateCallback callback_;
    std::atomic<bool> reload_requested_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::jthread thread_;
};

#endif

ThemeMonitor::ThemeMonitor(
    ThemePaths paths,
    ThemeCommandRunner run_command,
    ThemeSnapshot initial,
    ThemePreference preference,
    UpdateCallback callback)
    : impl_(std::make_unique<Impl>(
          std::move(paths),
          std::move(run_command),
          std::move(initial),
          preference,
          std::move(callback))) {}

ThemeMonitor::~ThemeMonitor() = default;

bool ThemeMonitor::available() const {
    return impl_->available();
}

void ThemeMonitor::set_theme_preference(ThemePreference preference) {
    impl_->set_theme_preference(preference);
}

}  // namespace su::app
