module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>

export module su.app.widgets.panel;

import std;
import su.app.ui.theme;

export namespace su::app::widgets {

void render_panel(EUINEO::UIContext& ui, const std::string& id, const EUINEO::RectFrame& frame);

}  // namespace su::app::widgets

namespace su::app::widgets {

void render_panel(EUINEO::UIContext& ui, const std::string& id, const EUINEO::RectFrame& frame) {
    ui.panel(id)
        .position(frame.x, frame.y)
        .size(frame.width, frame.height)
        .background(ui::panel_color())
        .gradient(EUINEO::RectGradient::Vertical(ui::panel_color(), ui::panel_alt_color()))
        .border(1.0f, ui::border_color())
        .rounding(ui::kPanelRadius)
        .build();
}

}  // namespace su::app::widgets
