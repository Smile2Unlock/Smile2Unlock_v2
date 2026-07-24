module;

#include <nlohmann/json.hpp>

export module su.app.preferences;

import std;

export namespace su::app {

enum class ThemePreference {
    system,
    light,
    dark,
};

enum class WindowControlsPreference {
    automatic,
    visible,
    hidden,
};

struct UiPreferences {
    std::optional<std::string> language;
    ThemePreference theme = ThemePreference::system;
    WindowControlsPreference window_controls = WindowControlsPreference::automatic;

    auto operator<=>(const UiPreferences&) const = default;
};

struct DesktopEnvironment {
    std::string current_desktop{};
    std::string session_desktop{};
    std::string desktop_session{};

    auto operator<=>(const DesktopEnvironment&) const = default;
};

[[nodiscard]] DesktopEnvironment current_desktop_environment();
[[nodiscard]] bool is_standalone_window_manager(const DesktopEnvironment& environment);
[[nodiscard]] bool uses_gnome_desktop(const DesktopEnvironment& environment);
[[nodiscard]] bool window_controls_visible(
    WindowControlsPreference preference,
    const DesktopEnvironment& environment);
std::expected<UiPreferences, std::string> load_ui_preferences(
    const std::filesystem::path& path);
std::expected<void, std::string> save_ui_preferences(
    const std::filesystem::path& path,
    const UiPreferences& preferences);

}  // namespace su::app

namespace su::app {

namespace {

using Json = nlohmann::json;

std::string environment_value(const char* name) {
    const auto* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
}

std::vector<std::string> desktop_tokens(const DesktopEnvironment& environment) {
    auto combined = std::format(
        "{}:{}:{}",
        environment.current_desktop,
        environment.session_desktop,
        environment.desktop_session);
    std::ranges::transform(combined, combined.begin(), [](unsigned char character) {
        const auto lowered = static_cast<char>(std::tolower(character));
        return std::isalnum(character) || lowered == '-' || lowered == '_' ? lowered : ':';
    });

    auto tokens = std::vector<std::string>{};
    auto start = std::size_t{0};
    while (start < combined.size()) {
        const auto end = combined.find(':', start);
        const auto length = end == std::string::npos ? combined.size() - start : end - start;
        if (length > 0) {
            tokens.emplace_back(combined.substr(start, length));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return tokens;
}

bool has_desktop_token(
    const DesktopEnvironment& environment,
    std::span<const std::string_view> candidates) {
    const auto tokens = desktop_tokens(environment);
    return std::ranges::any_of(tokens, [candidates](const auto& token) {
        return std::ranges::any_of(candidates, [&token](std::string_view candidate) {
            return candidate == token;
        });
    });
}

std::expected<ThemePreference, std::string> parse_theme_preference(const Json& value) {
    if (!value.is_string()) {
        return std::unexpected("theme_mode must be a string");
    }
    const auto text = value.get<std::string>();
    if (text == "system") {
        return ThemePreference::system;
    }
    if (text == "light") {
        return ThemePreference::light;
    }
    if (text == "dark") {
        return ThemePreference::dark;
    }
    return std::unexpected(std::format("unsupported theme_mode '{}'", text));
}

std::expected<WindowControlsPreference, std::string> parse_window_controls_preference(
    const Json& value) {
    if (!value.is_string()) {
        return std::unexpected("window_controls must be a string");
    }
    const auto text = value.get<std::string>();
    if (text == "auto") {
        return WindowControlsPreference::automatic;
    }
    if (text == "show") {
        return WindowControlsPreference::visible;
    }
    if (text == "hide") {
        return WindowControlsPreference::hidden;
    }
    return std::unexpected(std::format("unsupported window_controls '{}'", text));
}

std::string_view theme_preference_name(ThemePreference preference) {
    switch (preference) {
        case ThemePreference::system:
            return "system";
        case ThemePreference::light:
            return "light";
        case ThemePreference::dark:
            return "dark";
    }
    return "system";
}

std::string_view window_controls_preference_name(WindowControlsPreference preference) {
    switch (preference) {
        case WindowControlsPreference::automatic:
            return "auto";
        case WindowControlsPreference::visible:
            return "show";
        case WindowControlsPreference::hidden:
            return "hide";
    }
    return "auto";
}

}  // namespace

DesktopEnvironment current_desktop_environment() {
    return DesktopEnvironment{
        .current_desktop = environment_value("XDG_CURRENT_DESKTOP"),
        .session_desktop = environment_value("XDG_SESSION_DESKTOP"),
        .desktop_session = environment_value("DESKTOP_SESSION"),
    };
}

bool is_standalone_window_manager(const DesktopEnvironment& environment) {
    static constexpr auto standalone = std::array<std::string_view, 19>{
        "awesome",
        "bspwm",
        "dwm",
        "fluxbox",
        "herbstluftwm",
        "hyprland",
        "i3",
        "labwc",
        "niri",
        "openbox",
        "qtile",
        "river",
        "sway",
        "wayfire",
        "wmaker",
        "xmonad",
        "leftwm",
        "spectrwm",
        "icewm",
    };
    return has_desktop_token(environment, standalone);
}

bool uses_gnome_desktop(const DesktopEnvironment& environment) {
    static constexpr auto gnome = std::array<std::string_view, 4>{
        "gnome",
        "ubuntu",
        "budgie",
        "unity",
    };
    return has_desktop_token(environment, gnome);
}

bool window_controls_visible(
    WindowControlsPreference preference,
    const DesktopEnvironment& environment) {
    switch (preference) {
        case WindowControlsPreference::visible:
            return true;
        case WindowControlsPreference::hidden:
            return false;
        case WindowControlsPreference::automatic:
            return !is_standalone_window_manager(environment);
    }
    return true;
}

std::expected<UiPreferences, std::string> load_ui_preferences(
    const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return UiPreferences{};
    }
    try {
        auto stream = std::ifstream(path, std::ios::binary);
        if (!stream) {
            return std::unexpected(std::format("cannot open {}", path.string()));
        }
        const auto document = Json::parse(stream);
        if (!document.is_object()) {
            return std::unexpected(std::format("invalid UI preference file: {}", path.string()));
        }

        auto preferences = UiPreferences{};
        if (document.contains("language")) {
            if (!document["language"].is_string()) {
                return std::unexpected("language preference must be a string");
            }
            auto language = document["language"].get<std::string>();
            if (!language.empty()) {
                preferences.language = std::move(language);
            }
        }
        if (document.contains("theme_mode")) {
            const auto parsed = parse_theme_preference(document["theme_mode"]);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            preferences.theme = *parsed;
        }
        if (document.contains("window_controls")) {
            const auto parsed = parse_window_controls_preference(document["window_controls"]);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            preferences.window_controls = *parsed;
        }
        return preferences;
    } catch (const std::exception& error) {
        return std::unexpected(std::format("failed to load {}: {}", path.string(), error.what()));
    }
}

std::expected<void, std::string> save_ui_preferences(
    const std::filesystem::path& path,
    const UiPreferences& preferences) {
    try {
        std::filesystem::create_directories(path.parent_path());
        auto document = Json{
            {"theme_mode", theme_preference_name(preferences.theme)},
            {"window_controls", window_controls_preference_name(preferences.window_controls)},
        };
        if (preferences.language) {
            document["language"] = *preferences.language;
        }

        auto temporary = path;
        temporary += ".tmp";
        {
            auto stream = std::ofstream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) {
                return std::unexpected(std::format("cannot write {}", temporary.string()));
            }
            stream << document.dump(2) << '\n';
            if (!stream) {
                return std::unexpected(std::format("failed to write {}", temporary.string()));
            }
        }
        std::filesystem::permissions(
            temporary,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
        auto error = std::error_code{};
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
        }
        if (error) {
            return std::unexpected(std::format("failed to replace {}: {}", path.string(), error.message()));
        }
        return {};
    } catch (const std::exception& error) {
        return std::unexpected(std::format("failed to save {}: {}", path.string(), error.what()));
    }
}

}  // namespace su::app
