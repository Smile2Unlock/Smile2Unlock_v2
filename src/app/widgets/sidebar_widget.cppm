module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>

export module su.app.widgets.sidebar;

import std;
import su.app.presenters;
import su.app.state;
import su.app.ui.text;
import su.app.ui.theme;
import su.app.widgets.panel;

export namespace su::app::widgets {

void render_sidebar(EUINEO::UIContext& ui, const EUINEO::RectFrame& frame, const presenters::SidebarViewModel& sidebar);

}  // namespace su::app::widgets

namespace su::app::widgets {

void render_sidebar(EUINEO::UIContext& ui, const EUINEO::RectFrame& frame, const presenters::SidebarViewModel& sidebar) {
    render_panel(ui, "sidebar.panel", frame);

    const auto inner = ui::inset_frame(frame, ui::kPanelPadding);
    ui.label("sidebar.title")
        .position(inner.x, inner.y + 10.0f)
        .fontSize(20.0f)
        .text(ui::fit_text(sidebar.title, inner.width, 20.0f))
        .build();
    ui.label("sidebar.subtitle")
        .position(inner.x, inner.y + 38.0f)
        .fontSize(14.0f)
        .color(ui::muted_color())
        .text(ui::fit_text(sidebar.subtitle, inner.width, 14.0f))
        .build();

    auto current_y = inner.y + 68.0f;
    std::ranges::for_each(sidebar.items, [&](const auto& item) {
        ui.button(item.id)
            .position(inner.x, current_y)
            .size(inner.width, ui::kNavHeight)
            .text(ui::fit_text(item.label, inner.width - 20.0f, 18.0f))
            .style(item.active ? EUINEO::ButtonStyle::Primary : EUINEO::ButtonStyle::Outline)
            .onClick([page = item.page] {
                update_state([page](const AppState& state) {
                    return with_active_page(state, page);
                });
            })
            .build();
        current_y += ui::kNavHeight + ui::kNavGap;
    });
}

}  // namespace su::app::widgets
