module;

#include <eui/app/DslAppRuntime.h>

export module su.app.ui.theme;

import std;

export namespace su::app::ui {

using EUINEO::Color;
using EUINEO::RectFrame;

constexpr int kWindowWidth = 1360;
constexpr int kWindowHeight = 860;
constexpr float kOuterMargin = 14.0f;
constexpr float kSectionGap = 12.0f;
constexpr float kHeaderHeight = 86.0f;
constexpr float kSidebarWidth = 228.0f;
constexpr float kPanelRadius = 18.0f;
constexpr float kPanelPadding = 14.0f;
constexpr float kNavHeight = 42.0f;
constexpr float kNavGap = 8.0f;
constexpr float kCardHeight = 108.0f;
constexpr float kMinCardWidthForTwoColumns = 760.0f;

Color stage_color();
Color stage_glow();
Color panel_color();
Color panel_alt_color();
Color border_color();
Color accent_color();
Color muted_color();
RectFrame inset_frame(const RectFrame& frame, float inset);

}  // namespace su::app::ui

namespace su::app::ui {

Color stage_color() {
    return Color(0.08f, 0.10f, 0.14f, 1.0f);
}

Color stage_glow() {
    return Color(0.14f, 0.19f, 0.28f, 1.0f);
}

Color panel_color() {
    return Color(0.14f, 0.17f, 0.22f, 0.98f);
}

Color panel_alt_color() {
    return Color(0.17f, 0.20f, 0.26f, 0.98f);
}

Color border_color() {
    return Color(0.33f, 0.41f, 0.54f, 0.55f);
}

Color accent_color() {
    return Color(0.53f, 0.76f, 0.95f, 1.0f);
}

Color muted_color() {
    return Color(0.73f, 0.79f, 0.87f, 0.88f);
}

RectFrame inset_frame(const RectFrame& frame, float inset) {
    return RectFrame{
        frame.x + inset,
        frame.y + inset,
        std::max(0.0f, frame.width - inset * 2.0f),
        std::max(0.0f, frame.height - inset * 2.0f),
    };
}

}  // namespace su::app::ui
