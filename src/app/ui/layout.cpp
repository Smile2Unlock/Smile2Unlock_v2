#include "layout.h"

#include <eui/EUINEO.h>

#include "../app_state.h"
#include "../i18n/i18n.h"

import std;

namespace su::app::ui {
namespace {

using EUINEO::ButtonStyle;
using EUINEO::RectGradient;
using EUINEO::Renderer;

std::string tr(std::string_view key) {
    return i18n::text(i18n::app_i18n(), app_state().active_locale, key);
}

template <typename... Args>
std::string trf(std::string_view key, Args&&... args) {
    return i18n::format(i18n::app_i18n(), app_state().active_locale, key, std::forward<Args>(args)...);
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

void compose_card(UIContext& ui, const std::string& id, const RectFrame& frame, const PageCardContent& card) {
    compose_panel(ui, id, frame);

    const auto inner = inset_frame(frame, kPanelPadding);
    ui.label(id + ".title")
        .position(inner.x, inner.y + 6.0f)
        .fontSize(18.0f)
        .text(fit_text(card.title, inner.width, 20.0f))
        .build();
    ui.label(id + ".body")
        .position(inner.x, inner.y + 34.0f)
        .fontSize(16.0f)
        .color(muted_color())
        .text(fit_text(card.body, inner.width, 17.0f))
        .build();
}

}  // namespace

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

const std::array<NavItem, 5>& nav_items() {
    static const std::array<NavItem, 5> items = {{
        {PageId::Dashboard, "nav.dashboard"},
        {PageId::Enrollment, "nav.enrollment"},
        {PageId::Recognition, "nav.recognition"},
        {PageId::Settings, "nav.settings"},
        {PageId::Status, "nav.status"},
    }};
    return items;
}

void compose_header(UIContext& ui, const RectFrame& frame) {
    compose_panel(ui, "header.panel", frame);

    const auto inner = inset_frame(frame, kPanelPadding);
    const bool show_locale_switch = frame.width >= 880.0f;
    const float brand_width = show_locale_switch ? frame.width - 224.0f - kPanelPadding * 3.0f : inner.width;
    ui.label("header.brand")
        .position(inner.x, inner.y + 10.0f)
        .fontSize(26.0f)
        .text(fit_text(tr("app.title"), brand_width, 26.0f))
        .build();
    ui.label("header.subtitle")
        .position(inner.x, inner.y + 36.0f)
        .fontSize(15.0f)
        .color(muted_color())
        .text(fit_text(trf("header.subtitle", i18n::locale_display_name(i18n::app_i18n(), app_state().active_locale)), brand_width, 15.0f))
        .build();

    if (show_locale_switch) {
        const RectFrame switcher{
            frame.x + frame.width - 224.0f - kPanelPadding,
            frame.y + (frame.height - 38.0f) * 0.5f,
            224.0f,
            38.0f,
        };
        ui.button("header.locale.switch")
            .position(switcher.x, switcher.y)
            .size(switcher.width, switcher.height)
            .text(fit_text(trf("header.locale_button", i18n::locale_display_name(i18n::app_i18n(), app_state().active_locale)), switcher.width - 24.0f, 15.0f))
            .style(ButtonStyle::Outline)
            .onClick([] {
                update_state([](const AppState& state) {
                    return with_active_locale(state, i18n::cycle_locale(i18n::app_i18n(), state.active_locale));
                });
            })
            .build();
    }
}

void compose_sidebar(UIContext& ui, const RectFrame& frame) {
    compose_panel(ui, "sidebar.panel", frame);

    const auto inner = inset_frame(frame, kPanelPadding);
    ui.label("sidebar.title")
        .position(inner.x, inner.y + 10.0f)
        .fontSize(20.0f)
        .text(fit_text(tr("sidebar.title"), inner.width, 20.0f))
        .build();
    ui.label("sidebar.subtitle")
        .position(inner.x, inner.y + 38.0f)
        .fontSize(14.0f)
        .color(muted_color())
        .text(fit_text(tr("sidebar.subtitle"), inner.width, 14.0f))
        .build();

    float current_y = inner.y + 68.0f;
    for (const auto& item : nav_items()) {
        const bool active = app_state().active_page == item.page;
        ui.button(std::format("sidebar.{}", page_name(item.page)))
            .position(inner.x, current_y)
            .size(inner.width, kNavHeight)
            .text(fit_text(tr(item.key), inner.width - 20.0f, 18.0f))
            .style(active ? ButtonStyle::Primary : ButtonStyle::Outline)
            .onClick([page = item.page] {
                update_state([page](const AppState& state) {
                    return with_active_page(state, page);
                });
            })
            .build();
        current_y += kNavHeight + kNavGap;
    }
}

void compose_main_content(UIContext& ui, const RectFrame& frame, const PageContent& page) {
    compose_panel(ui, "content.panel", frame);

    const auto inner = inset_frame(frame, kPanelPadding);
    ui.label("content.title")
        .position(inner.x, inner.y + 10.0f)
        .fontSize(24.0f)
        .text(fit_text(page.title, inner.width, 24.0f))
        .build();
    ui.label("content.subtitle")
        .position(inner.x, inner.y + 42.0f)
        .fontSize(16.0f)
        .color(muted_color())
        .text(fit_text(page.description, inner.width, 16.0f))
        .build();

    const float cards_y = inner.y + 76.0f;
    const bool use_two_columns = inner.width >= kMinCardWidthForTwoColumns;
    const float cards_gap = kSectionGap;
    float stage_y = 0.0f;
    if (use_two_columns) {
        const float cards_width = (inner.width - cards_gap) * 0.5f;
        compose_card(ui, "content.card.primary", RectFrame{inner.x, cards_y, cards_width, kCardHeight}, page.primary_card);
        compose_card(ui, "content.card.secondary", RectFrame{inner.x + cards_width + cards_gap, cards_y, cards_width, kCardHeight}, page.secondary_card);
        stage_y = cards_y + kCardHeight + kSectionGap;
    } else {
        compose_card(ui, "content.card.primary", RectFrame{inner.x, cards_y, inner.width, kCardHeight}, page.primary_card);
        compose_card(ui, "content.card.secondary", RectFrame{inner.x, cards_y + kCardHeight + cards_gap, inner.width, kCardHeight}, page.secondary_card);
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
        .text(fit_text(page.stage_title, stage.width - 44.0f, 21.0f))
        .build();
    ui.label("content.stage.body")
        .position(stage.x + 22.0f, stage.y + 60.0f)
        .fontSize(16.0f)
        .color(muted_color())
        .text(fit_text(page.stage_body, stage.width - 44.0f, 16.0f))
        .build();
    ui.label("content.stage.diagnostics")
        .position(stage.x + 22.0f, stage.y + 90.0f)
        .fontSize(14.0f)
        .color(muted_color())
        .text(fit_text(trf("content.diagnostics.body", i18n::diagnostics_text(i18n::app_i18n())), stage.width - 44.0f, 14.0f))
        .build();
}

}  // namespace su::app::ui
