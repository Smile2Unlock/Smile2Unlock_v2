module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>

export module su.app.widgets.header;

import std;
import su.app.i18n;
import su.app.presenters;
import su.app.state;
import su.app.ui.theme;
import su.app.widgets.panel;

export namespace su::app::widgets {

void render_header(EUINEO::UIContext& ui, const EUINEO::RectFrame& frame, const presenters::HeaderViewModel& header);

}  // namespace su::app::widgets

namespace su::app::widgets {

void render_header(EUINEO::UIContext& ui, const EUINEO::RectFrame& frame, const presenters::HeaderViewModel& header) {
    render_panel(ui, "header.panel", frame);

    const float padding = 16.0f;
    const auto inner = EUINEO::RectFrame{frame.x + padding, frame.y + padding, frame.width - padding * 2.0f, frame.height - padding * 2.0f};

    ui.label("header.brand")
        .position(inner.x, inner.y + 8.0f)
        .fontSize(20.0f)
        .color(ui::text_primary_color())
        .text(header.title)
        .build();
    ui.label("header.subtitle")
        .position(inner.x, inner.y + 34.0f)
        .fontSize(14.0f)
        .color(ui::text_secondary_color())
        .text(header.subtitle)
        .build();

    if (!header.show_locale_switch) {
        return;
    }

    const EUINEO::RectFrame switcher{
        frame.x + frame.width - 240.0f - padding,
        frame.y + (frame.height - 36.0f) * 0.5f,
        240.0f,
        36.0f,
    };
    ui.button("header.locale.switch")
        .position(switcher.x, switcher.y)
        .size(switcher.width, switcher.height)
        .text(header.locale_button_text)
        .style(EUINEO::ButtonStyle::Outline)
        .onClick([]() {
            update_state([](const AppState& state) {
                return with_active_locale(state, i18n::cycle_locale(i18n::app_i18n(), state.active_locale));
            });
        })
        .build();
}

}  // namespace su::app::widgets