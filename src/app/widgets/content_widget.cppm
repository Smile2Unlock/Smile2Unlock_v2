module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>

export module su.app.widgets.content;

import std;
import su.app.pages;
import su.app.presenters;
import su.app.ui.text;
import su.app.ui.theme;
import su.app.widgets.panel;

export namespace su::app::widgets {

void render_content(EUINEO::UIContext& ui, const EUINEO::RectFrame& frame, const presenters::ContentViewModel& content);

}  // namespace su::app::widgets

namespace su::app::widgets {
namespace {

void render_card(
    EUINEO::UIContext& ui,
    const std::string& id,
    const EUINEO::RectFrame& frame,
    const su::app::PageCardContent& card
) {
    render_panel(ui, id, frame);

    const auto inner = ui::inset_frame(frame, ui::kPanelPadding);
    ui.label(id + ".title")
        .position(inner.x, inner.y + 6.0f)
        .fontSize(18.0f)
        .text(ui::fit_text(card.title, inner.width, 20.0f))
        .build();
    ui.label(id + ".body")
        .position(inner.x, inner.y + 34.0f)
        .fontSize(16.0f)
        .color(ui::muted_color())
        .text(ui::fit_text(card.body, inner.width, 17.0f))
        .build();
}

}  // namespace

void render_content(EUINEO::UIContext& ui, const EUINEO::RectFrame& frame, const presenters::ContentViewModel& content) {
    render_panel(ui, "content.panel", frame);

    const auto inner = ui::inset_frame(frame, ui::kPanelPadding);
    ui.label("content.title")
        .position(inner.x, inner.y + 10.0f)
        .fontSize(24.0f)
        .text(ui::fit_text(content.title, inner.width, 24.0f))
        .build();
    ui.label("content.subtitle")
        .position(inner.x, inner.y + 42.0f)
        .fontSize(16.0f)
        .color(ui::muted_color())
        .text(ui::fit_text(content.description, inner.width, 16.0f))
        .build();

    const float cards_y = inner.y + 76.0f;
    const bool use_two_columns = inner.width >= ui::kMinCardWidthForTwoColumns;
    const float cards_gap = ui::kSectionGap;
    const auto stage_y = [&] {
        if (use_two_columns) {
            const float cards_width = (inner.width - cards_gap) * 0.5f;
            render_card(ui, "content.card.primary", EUINEO::RectFrame{inner.x, cards_y, cards_width, ui::kCardHeight}, content.primary_card);
            render_card(ui, "content.card.secondary", EUINEO::RectFrame{inner.x + cards_width + cards_gap, cards_y, cards_width, ui::kCardHeight}, content.secondary_card);
            return cards_y + ui::kCardHeight + ui::kSectionGap;
        }

        render_card(ui, "content.card.primary", EUINEO::RectFrame{inner.x, cards_y, inner.width, ui::kCardHeight}, content.primary_card);
        render_card(ui, "content.card.secondary", EUINEO::RectFrame{inner.x, cards_y + ui::kCardHeight + cards_gap, inner.width, ui::kCardHeight}, content.secondary_card);
        return cards_y + (ui::kCardHeight * 2.0f) + (ui::kSectionGap * 2.0f);
    }();

    const float stage_height = std::max(120.0f, inner.height - (stage_y - inner.y));
    const EUINEO::RectFrame stage{inner.x, stage_y, inner.width, stage_height};
    ui.panel("content.stage")
        .position(stage.x, stage.y)
        .size(stage.width, stage.height)
        .background(ui::panel_color())
        .gradient(EUINEO::RectGradient::Corners(ui::stage_glow(), ui::panel_alt_color(), ui::panel_color(), ui::panel_alt_color()))
        .border(1.0f, ui::border_color())
        .rounding(16.0f)
        .build();
    ui.label("content.stage.title")
        .position(stage.x + 22.0f, stage.y + 30.0f)
        .fontSize(21.0f)
        .color(ui::accent_color())
        .text(ui::fit_text(content.stage_title, stage.width - 44.0f, 21.0f))
        .build();
    ui.label("content.stage.body")
        .position(stage.x + 22.0f, stage.y + 60.0f)
        .fontSize(16.0f)
        .color(ui::muted_color())
        .text(ui::fit_text(content.stage_body, stage.width - 44.0f, 16.0f))
        .build();
    ui.label("content.stage.diagnostics")
        .position(stage.x + 22.0f, stage.y + 90.0f)
        .fontSize(14.0f)
        .color(ui::muted_color())
        .text(ui::fit_text(content.diagnostics, stage.width - 44.0f, 14.0f))
        .build();
}

}  // namespace su::app::widgets
