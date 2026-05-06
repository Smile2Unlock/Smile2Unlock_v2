module;

#include <core/primitive.h>

export module su.app.ui.theme;

import std;

export namespace su::app::ui {

// Window size constants
constexpr int kWindowWidth = 1440;
constexpr int kWindowHeight = 900;
constexpr float kSidebarWidth = 272.0f;

// Color helpers (using core::Color from v0.3.6)
core::Color accent_primary_color();
core::Color accent_success_color();
core::Color accent_warning_color();
core::Color accent_danger_color();

}  // namespace su::app::ui

namespace su::app::ui {

core::Color accent_primary_color() {
    return core::Color{0.30f, 0.55f, 0.95f, 1.0f};
}

core::Color accent_success_color() {
    return core::Color{0.20f, 0.75f, 0.45f, 1.0f};
}

core::Color accent_warning_color() {
    return core::Color{0.90f, 0.65f, 0.20f, 1.0f};
}

core::Color accent_danger_color() {
    return core::Color{0.85f, 0.25f, 0.30f, 1.0f};
}

}  // namespace su::app::ui
