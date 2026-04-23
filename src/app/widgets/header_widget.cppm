module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>

export module su.app.widgets.header;

import std;
import su.app.i18n;
import su.app.presenters;
import su.app.state;
import su.app.ui.text;
import su.app.ui.theme;
import su.app.widgets.panel;

export namespace su::app::widgets {

void render_header(EUINEO::UIContext& ui, const EUINEO::RectFrame& frame, const presenters::HeaderViewModel& header);

}  // namespace su::app::widgets

namespace su::app::widgets {

void render_header(EUINEO::UIContext& ui, const EUINEO::RectFrame& frame, const presenters::HeaderViewModel& header) {
    render_panel(ui, "header.panel", frame);

    const auto inner = ui::inset_frame(frame, ui::kPanelPadding);
    const float brand_width = header.show_locale_switch ? frame.width - 224.0f - ui::kPanelPadding * 3.0f : inner.width;
    ui.label("header.brand")
        .position(inner.x, inner.y + 10.0f)
        .fontSize(26.0f)
        .text(ui::fit_text(header.title, brand_width, 26.0f))
        .build();
    ui.label("header.subtitle")
        .position(inner.x, inner.y + 36.0f)
        .fontSize(15.0f)
        .color(ui::muted_color())
        .text(ui::fit_text(header.subtitle, brand_width, 15.0f))
        .build();

    if (!header.show_locale_switch) {
        return;
    }

    const EUINEO::RectFrame switcher{
        frame.x + frame.width - 224.0f - ui::kPanelPadding,
        frame.y + (frame.height - 38.0f) * 0.5f,
        224.0f,
        38.0f,
    };
    ui.button("header.locale.switch")
        .position(switcher.x, switcher.y)
        .size(switcher.width, switcher.height)
        .text(ui::fit_text(header.locale_button_text, switcher.width - 24.0f, 15.0f))
        .style(EUINEO::ButtonStyle::Outline)
        .onClick([] {
            update_state([](const AppState& state) {
                return with_active_locale(state, i18n::cycle_locale(i18n::app_i18n(), state.active_locale));
            });
        })
        .build();
}

}  // namespace su::app::widgets
