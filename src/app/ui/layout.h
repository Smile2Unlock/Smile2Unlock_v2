#pragma once

#include <eui/app/DslAppRuntime.h>

#include <array>
#include <string>
#include <string_view>

#include "../app_state.h"
#include "../pages/page_content.h"

namespace su::app::ui {

using EUINEO::Color;
using EUINEO::RectFrame;
using EUINEO::UIContext;

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

struct NavItem {
    PageId page;
    std::string_view key;
};

Color stage_color();
Color stage_glow();
Color panel_color();
Color panel_alt_color();
Color border_color();
Color accent_color();
Color muted_color();

std::string fit_text(std::string text, float max_width, float font_size);
RectFrame inset_frame(const RectFrame& frame, float inset);

const std::array<NavItem, 5>& nav_items();

void compose_header(UIContext& ui, const RectFrame& frame);
void compose_sidebar(UIContext& ui, const RectFrame& frame);
void compose_main_content(UIContext& ui, const RectFrame& frame, const PageContent& page);

}  // namespace su::app::ui
