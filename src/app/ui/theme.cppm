module;

#include <eui/app/DslAppRuntime.h>

export module su.app.ui.theme;

import std;

export namespace su::app::ui {

using EUINEO::Color;
using EUINEO::RectFrame;

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 800;
constexpr float kTitleBarHeight = 40.0f;
constexpr float kSidebarWidth = 208.0f;
constexpr float kSidebarCollapsedWidth = 56.0f;
constexpr float kContentPadding = 20.0f;
constexpr float kCardGap = 16.0f;
constexpr float kCardMinWidth = 280.0f;
constexpr float kWindowButtonSize = 28.0f;
constexpr float kWindowButtonGap = 8.0f;
constexpr float kSidebarToggleSize = 36.0f;
constexpr float kNavItemHeight = 44.0f;
constexpr float kNavItemGap = 4.0f;

Color title_bar_color();
Color title_bar_hover_color();
Color window_button_close_hover();
Color window_button_hover();
Color sidebar_color();
Color sidebar_hover_color();
Color sidebar_active_color();
Color content_bg_color();
Color card_bg_color();
Color card_hover_color();
Color text_primary_color();
Color text_secondary_color();
Color text_muted_color();
Color accent_primary_color();
Color accent_success_color();
Color accent_warning_color();
Color accent_danger_color();
Color border_light_color();

RectFrame make_title_bar_frame(const RectFrame& window);
RectFrame make_sidebar_frame(const RectFrame& window, bool sidebar_collapsed);
RectFrame make_content_frame(const RectFrame& window, bool sidebar_collapsed);

}  // namespace su::app::ui

namespace su::app::ui {

Color title_bar_color() {
    return Color(0.15f, 0.15f, 0.18f, 1.0f);
}

Color title_bar_hover_color() {
    return Color(0.20f, 0.20f, 0.24f, 1.0f);
}

Color window_button_close_hover() {
    return Color(0.85f, 0.23f, 0.23f, 1.0f);
}

Color window_button_hover() {
    return Color(0.35f, 0.35f, 0.38f, 1.0f);
}

Color sidebar_color() {
    return Color(0.18f, 0.18f, 0.22f, 1.0f);
}

Color sidebar_hover_color() {
    return Color(0.22f, 0.22f, 0.26f, 1.0f);
}

Color sidebar_active_color() {
    return Color(0.25f, 0.45f, 0.75f, 1.0f);
}

Color content_bg_color() {
    return Color(0.12f, 0.12f, 0.14f, 1.0f);
}

Color card_bg_color() {
    return Color(0.20f, 0.20f, 0.24f, 1.0f);
}

Color card_hover_color() {
    return Color(0.24f, 0.24f, 0.28f, 1.0f);
}

Color text_primary_color() {
    return Color(0.95f, 0.95f, 0.97f, 1.0f);
}

Color text_secondary_color() {
    return Color(0.70f, 0.72f, 0.75f, 1.0f);
}

Color text_muted_color() {
    return Color(0.50f, 0.52f, 0.55f, 1.0f);
}

Color accent_primary_color() {
    return Color(0.30f, 0.55f, 0.95f, 1.0f);
}

Color accent_success_color() {
    return Color(0.20f, 0.75f, 0.45f, 1.0f);
}

Color accent_warning_color() {
    return Color(0.90f, 0.65f, 0.20f, 1.0f);
}

Color accent_danger_color() {
    return Color(0.85f, 0.25f, 0.30f, 1.0f);
}

Color border_light_color() {
    return Color(0.35f, 0.36f, 0.40f, 0.60f);
}

RectFrame make_title_bar_frame(const RectFrame& window) {
    return RectFrame{0.0f, 0.0f, window.width, kTitleBarHeight};
}

RectFrame make_sidebar_frame(const RectFrame& window, bool sidebar_collapsed) {
    const auto sidebar_width = sidebar_collapsed ? kSidebarCollapsedWidth : kSidebarWidth;
    return RectFrame{0.0f, kTitleBarHeight, sidebar_width, window.height - kTitleBarHeight};
}

RectFrame make_content_frame(const RectFrame& window, bool sidebar_collapsed) {
    const auto sidebar_width = sidebar_collapsed ? kSidebarCollapsedWidth : kSidebarWidth;
    return RectFrame{sidebar_width, kTitleBarHeight, window.width - sidebar_width, window.height - kTitleBarHeight};
}

}  // namespace su::app::ui
