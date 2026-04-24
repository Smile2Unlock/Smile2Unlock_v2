module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>

export module su.app.widgets.content;

import std;
import su.app.pages;
import su.app.presenters;
import su.app.ui.theme;
import su.app.widgets.panel;

export namespace su::app::widgets {

void render_content(EUINEO::UIContext& ui, const EUINEO::RectFrame& frame, const presenters::ContentViewModel& content);

}  // namespace su::app::widgets

namespace su::app::widgets {
namespace {

using EUINEO::RectFrame;
using EUINEO::UIContext;

void render_card(
    EUINEO::UIContext& ui,
    const std::string& id,
    const EUINEO::RectFrame& frame,
    const su::app::PageCardContent& card
) {
    render_panel(ui, id, frame);

    const float padding = 16.0f;
    ui.label(id + ".title")
        .position(frame.x + padding, frame.y + padding)
        .fontSize(16.0f)
        .color(ui::text_primary_color())
        .text(card.title)
        .build();
    ui.label(id + ".body")
        .position(frame.x + padding, frame.y + padding + 28.0f)
        .fontSize(13.0f)
        .color(ui::text_secondary_color())
        .text(card.body)
        .build();
}

}  // namespace

void render_content(UIContext& ui, const RectFrame& frame, const presenters::ContentViewModel& content) {
    render_panel(ui, "content.panel", frame);

    const float padding = 20.0f;
    ui.label("content.title")
        .position(frame.x + padding, frame.y + padding)
        .fontSize(24.0f)
        .color(ui::text_primary_color())
        .text(content.title)
        .build();
    ui.label("content.subtitle")
        .position(frame.x + padding, frame.y + padding + 32.0f)
        .fontSize(14.0f)
        .color(ui::text_secondary_color())
        .text(content.description)
        .build();

    const float cards_y = frame.y + padding + 72.0f;
    const float card_width = (frame.width - padding * 2.0f - ui::kCardGap) / 2.0f;
    const float card_height = 100.0f;

    render_card(ui, "content.card.primary", RectFrame{frame.x + padding, cards_y, card_width, card_height}, content.primary_card);
    render_card(ui, "content.card.secondary", RectFrame{frame.x + padding + card_width + ui::kCardGap, cards_y, card_width, card_height}, content.secondary_card);

    const float stage_y = cards_y + card_height + ui::kCardGap;
    const float stage_height = std::max(120.0f, frame.height - (stage_y - frame.y) - padding);
    const RectFrame stage{frame.x + padding, stage_y, frame.width - padding * 2.0f, stage_height};

    ui.panel("content.stage")
        .position(stage.x, stage.y)
        .size(stage.width, stage.height)
        .background(ui::card_bg_color())
        .rounding(12.0f)
        .border(1.0f, ui::border_light_color())
        .build();
    ui.label("content.stage.title")
        .position(stage.x + 16.0f, stage.y + 16.0f)
        .fontSize(18.0f)
        .color(ui::accent_primary_color())
        .text(content.stage_title)
        .build();
    ui.label("content.stage.body")
        .position(stage.x + 16.0f, stage.y + 44.0f)
        .fontSize(14.0f)
        .color(ui::text_secondary_color())
        .text(content.stage_body)
        .build();
    ui.label("content.stage.diagnostics")
        .position(stage.x + 16.0f, stage.y + 72.0f)
        .fontSize(12.0f)
        .color(ui::text_muted_color())
        .text(content.diagnostics)
        .build();
}

}  // namespace su::app::widgets