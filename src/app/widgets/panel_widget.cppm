module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>

export module su.app.widgets.panel;

import std;
import su.app.ui.theme;

export namespace su::app::widgets {

void render_panel(EUINEO::UIContext& ui, const std::string& id, const EUINEO::RectFrame& frame);
void render_card(EUINEO::UIContext& ui, const std::string& id, const EUINEO::RectFrame& frame);

}  // namespace su::app::widgets

namespace su::app::widgets {

void render_panel(EUINEO::UIContext& ui, const std::string& id, const EUINEO::RectFrame& frame) {
    ui.panel(id)
        .position(frame.x, frame.y)
        .size(frame.width, frame.height)
        .background(ui::content_bg_color())
        .build();
}

void render_card(EUINEO::UIContext& ui, const std::string& id, const EUINEO::RectFrame& frame) {
    ui.panel(id)
        .position(frame.x, frame.y)
        .size(frame.width, frame.height)
        .background(ui::card_bg_color())
        .rounding(12.0f)
        .border(1.0f, ui::border_light_color())
        .build();
}

}  // namespace su::app::widgets