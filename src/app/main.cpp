#include <eui/app/DslAppRuntime.h>

import std;

namespace {

using EUINEO::ButtonStyle;
using EUINEO::Color;
using EUINEO::RectFrame;
using EUINEO::RectGradient;
using EUINEO::Renderer;
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

enum class PageId {
    Dashboard,
    Enrollment,
    Recognition,
    Settings,
    Status,
};

struct NavItem {
    PageId page;
    std::string_view title;
};

struct AppState {
    PageId active_page = PageId::Dashboard;
};

AppState& app_state() {
    static AppState state;
    return state;
}

const std::array<NavItem, 5>& nav_items() {
    static const std::array<NavItem, 5> items = {{
        {PageId::Dashboard, "Dashboard"},
        {PageId::Enrollment, "Enrollment"},
        {PageId::Recognition, "Recognition"},
        {PageId::Settings, "Settings"},
        {PageId::Status, "Status"},
    }};
    return items;
}

std::string page_title(PageId page) {
    switch (page) {
        case PageId::Dashboard:
            return "Dashboard";
        case PageId::Enrollment:
            return "Enrollment";
        case PageId::Recognition:
            return "Recognition";
        case PageId::Settings:
            return "Settings";
        case PageId::Status:
            return "Status";
    }
    return "Dashboard";
}

std::string page_description(PageId page) {
    switch (page) {
        case PageId::Dashboard:
            return "Overview page layout skeleton.";
        case PageId::Enrollment:
            return "User and face enrollment layout skeleton.";
        case PageId::Recognition:
            return "Recognition runtime layout skeleton.";
        case PageId::Settings:
            return "Configuration layout skeleton.";
        case PageId::Status:
            return "Health and diagnostics layout skeleton.";
    }
    return "Overview page layout skeleton.";
}

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

std::string fit_text(std::string text, float max_width, float font_size) {
    if (text.empty() || max_width <= 0.0f) {
        return {};
    }

    const float scale = font_size / 24.0f;
    if (Renderer::MeasureTextWidth(text, scale) <= max_width) {
        return text;
    }

    static constexpr std::string_view ellipsis = "...";
    while (!text.empty()) {
        text.pop_back();
        const auto candidate = text + std::string(ellipsis);
        if (Renderer::MeasureTextWidth(candidate, scale) <= max_width) {
            return candidate;
        }
    }
    return std::string(ellipsis);
}

RectFrame inset_frame(const RectFrame& frame, float inset) {
    return RectFrame{
        frame.x + inset,
        frame.y + inset,
        std::max(0.0f, frame.width - inset * 2.0f),
        std::max(0.0f, frame.height - inset * 2.0f),
    };
}

void compose_panel(UIContext& ui, const std::string& id, const RectFrame& frame) {
    ui.panel(id)
        .position(frame.x, frame.y)
        .size(frame.width, frame.height)
        .background(panel_color())
        .gradient(RectGradient::Vertical(panel_color(), panel_alt_color()))
        .border(1.0f, border_color())
        .rounding(kPanelRadius)
        .build();
}

void compose_card(
    UIContext& ui,
    const std::string& id,
    const RectFrame& frame,
    const std::string& title,
    const std::string& body
) {
    compose_panel(ui, id, frame);

    const auto inner = inset_frame(frame, kPanelPadding);
    const auto fitted_title = fit_text(title, inner.width, 20.0f);
    const auto fitted_body = fit_text(body, inner.width, 17.0f);
    ui.label(id + ".title")
        .position(inner.x, inner.y + 6.0f)
        .fontSize(18.0f)
        .text(fitted_title)
        .build();
    ui.label(id + ".body")
        .position(inner.x, inner.y + 34.0f)
        .fontSize(16.0f)
        .color(muted_color())
        .text(fitted_body)
        .build();
}

void compose_header(UIContext& ui, const RectFrame& frame) {
    compose_panel(ui, "header.panel", frame);

    const auto inner = inset_frame(frame, kPanelPadding);
    const bool show_badge = frame.width >= 880.0f;
    const float brand_width = show_badge ? frame.width - 204.0f - kPanelPadding * 3.0f : inner.width;
    ui.label("header.brand")
        .position(inner.x, inner.y + 10.0f)
        .fontSize(26.0f)
        .text(fit_text("Smile2Unlock", brand_width, 26.0f))
        .build();
    ui.label("header.subtitle")
        .position(inner.x, inner.y + 36.0f)
        .fontSize(15.0f)
        .color(muted_color())
        .text(fit_text("Layout rewrite in progress", brand_width, 15.0f))
        .build();

    if (show_badge) {
        const RectFrame badge{
            frame.x + frame.width - 204.0f - kPanelPadding,
            frame.y + (frame.height - 38.0f) * 0.5f,
            204.0f,
            38.0f,
        };
        ui.panel("header.badge")
            .position(badge.x, badge.y)
            .size(badge.width, badge.height)
            .background(Color(accent_color().r, accent_color().g, accent_color().b, 0.14f))
            .border(1.0f, Color(accent_color().r, accent_color().g, accent_color().b, 0.42f))
            .rounding(14.0f)
            .build();
        ui.label("header.badge.text")
            .position(badge.x + 20.0f, badge.y + 24.0f)
            .fontSize(15.0f)
            .color(accent_color())
            .text(fit_text("Layout skeleton only", badge.width - 40.0f, 15.0f))
            .build();
    }
}

void compose_sidebar(UIContext& ui, const RectFrame& frame) {
    compose_panel(ui, "sidebar.panel", frame);

    const auto inner = inset_frame(frame, kPanelPadding);
    const float nav_width = inner.width;
    ui.label("sidebar.title")
        .position(inner.x, inner.y + 10.0f)
        .fontSize(20.0f)
        .text(fit_text("Navigation", nav_width, 20.0f))
        .build();
    ui.label("sidebar.subtitle")
        .position(inner.x, inner.y + 38.0f)
        .fontSize(14.0f)
        .color(muted_color())
        .text(fit_text("Page shells first, details later.", nav_width, 14.0f))
        .build();

    float current_y = inner.y + 68.0f;
    for (const auto& item : nav_items()) {
        const bool active = app_state().active_page == item.page;
        ui.button(std::format("sidebar.{}", item.title))
            .position(inner.x, current_y)
            .size(inner.width, kNavHeight)
            .text(fit_text(std::string(item.title), nav_width - 20.0f, 18.0f))
            .style(active ? ButtonStyle::Primary : ButtonStyle::Outline)
            .onClick([page = item.page] { app_state().active_page = page; })
            .build();
        current_y += kNavHeight + kNavGap;
    }
}

void compose_main_content(UIContext& ui, const RectFrame& frame) {
    compose_panel(ui, "content.panel", frame);

    const auto inner = inset_frame(frame, kPanelPadding);
    const auto fitted_page_title = fit_text(page_title(app_state().active_page), inner.width, 24.0f);
    const auto fitted_subtitle = fit_text(page_description(app_state().active_page), inner.width, 16.0f);
    ui.label("content.title")
        .position(inner.x, inner.y + 10.0f)
        .fontSize(24.0f)
        .text(fitted_page_title)
        .build();
    ui.label("content.subtitle")
        .position(inner.x, inner.y + 42.0f)
        .fontSize(16.0f)
        .color(muted_color())
        .text(fitted_subtitle)
        .build();

    const float cards_y = inner.y + 76.0f;
    const bool use_two_columns = inner.width >= kMinCardWidthForTwoColumns;
    const float cards_gap = kSectionGap;
    float stage_y = 0.0f;
    if (use_two_columns) {
        const float cards_width = (inner.width - cards_gap) * 0.5f;
        compose_card(
            ui,
            "content.card.primary",
            RectFrame{inner.x, cards_y, cards_width, kCardHeight},
            "Primary region",
            "Main page widget group goes here."
        );
        compose_card(
            ui,
            "content.card.secondary",
            RectFrame{inner.x + cards_width + cards_gap, cards_y, cards_width, kCardHeight},
            "Secondary region",
            "Summary, actions, or contextual notes go here."
        );
        stage_y = cards_y + kCardHeight + kSectionGap;
    } else {
        compose_card(
            ui,
            "content.card.primary",
            RectFrame{inner.x, cards_y, inner.width, kCardHeight},
            "Primary region",
            "Main page widget group goes here."
        );
        compose_card(
            ui,
            "content.card.secondary",
            RectFrame{inner.x, cards_y + kCardHeight + cards_gap, inner.width, kCardHeight},
            "Secondary region",
            "Summary, actions, or contextual notes go here."
        );
        stage_y = cards_y + (kCardHeight * 2.0f) + (kSectionGap * 2.0f);
    }

    const float stage_height = std::max(120.0f, inner.height - (stage_y - inner.y));
    const RectFrame stage{inner.x, stage_y, inner.width, stage_height};
    ui.panel("content.stage")
        .position(stage.x, stage.y)
        .size(stage.width, stage.height)
        .background(Color(0.11f, 0.13f, 0.18f, 0.98f))
        .gradient(RectGradient::Corners(stage_glow(), panel_alt_color(), panel_color(), panel_alt_color()))
        .border(1.0f, border_color())
        .rounding(16.0f)
        .build();
    ui.label("content.stage.title")
        .position(stage.x + 22.0f, stage.y + 30.0f)
        .fontSize(21.0f)
        .color(accent_color())
        .text(fit_text("Page canvas", stage.width - 44.0f, 21.0f))
        .build();
    ui.label("content.stage.body")
        .position(stage.x + 22.0f, stage.y + 60.0f)
        .fontSize(16.0f)
        .color(muted_color())
        .text(fit_text("Components were reset. We can now rebuild each page without overflow.", stage.width - 44.0f, 16.0f))
        .build();
}

}  // namespace

int main() {
    EUINEO::DslAppConfig config;
    config.title = "Smile2Unlock";
    config.width = kWindowWidth;
    config.height = kWindowHeight;
    config.pageId = "smile2unlock";
    config.fps = 120;
    config.darkTitleBar = true;

    return EUINEO::RunDslApp(config, [](UIContext& ui, const RectFrame& screen) {
        EUINEO::UseDslDarkTheme(stage_color());

        ui.panel("stage")
            .position(0.0f, 0.0f)
            .size(screen.width, screen.height)
            .background(stage_color())
            .gradient(RectGradient::Corners(
                stage_glow(),
                stage_color(),
                Color(0.07f, 0.09f, 0.12f, 1.0f),
                Color(0.12f, 0.15f, 0.21f, 1.0f)))
            .build();

        const RectFrame header{
            kOuterMargin,
            kOuterMargin,
            screen.width - kOuterMargin * 2.0f,
            kHeaderHeight,
        };
        const RectFrame sidebar{
            kOuterMargin,
            header.y + header.height + kSectionGap,
            std::min(kSidebarWidth, screen.width * 0.24f),
            screen.height - header.height - kOuterMargin * 2.0f - kSectionGap,
        };
        const RectFrame content{
            sidebar.x + sidebar.width + kSectionGap,
            sidebar.y,
            std::max(320.0f, screen.width - sidebar.width - kOuterMargin * 2.0f - kSectionGap),
            sidebar.height,
        };

        compose_header(ui, header);
        compose_sidebar(ui, sidebar);
        compose_main_content(ui, content);
    });
}
