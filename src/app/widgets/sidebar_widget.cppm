module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>

export module su.app.widgets.sidebar;

import std;
import su.app.presenters;
import su.app.state;
import su.app.ui.theme;
import su.app.widgets.panel;

export namespace su::app::widgets {

void render_sidebar(EUINEO::UIContext& ui, const EUINEO::RectFrame& frame, const presenters::SidebarViewModel& sidebar);

}  // namespace su::app::widgets

namespace su::app::widgets {

void render_sidebar(EUINEO::UIContext& ui, const EUINEO::RectFrame& frame, const presenters::SidebarViewModel& sidebar) {
    render_panel(ui, "sidebar.panel", frame);

    const float padding = 16.0f;
    const auto inner = EUINEO::RectFrame{frame.x + padding, frame.y + padding, frame.width - padding * 2.0f, frame.height - padding * 2.0f};

    ui.label("sidebar.title")
        .position(inner.x, inner.y + 10.0f)
        .fontSize(20.0f)
        .color(ui::text_primary_color())
        .text(sidebar.title)
        .build();
    ui.label("sidebar.subtitle")
        .position(inner.x, inner.y + 38.0f)
        .fontSize(14.0f)
        .color(ui::text_secondary_color())
        .text(sidebar.subtitle)
        .build();

    auto current_y = inner.y + 68.0f;
    std::ranges::for_each(sidebar.items, [&](const auto& item) {
        ui.button(item.id)
            .position(inner.x, current_y)
            .size(inner.width, ui::kNavItemHeight)
            .text(item.label)
            .style(item.active ? EUINEO::ButtonStyle::Primary : EUINEO::ButtonStyle::Default)
            .onClick([page = item.page]() {
                update_state([page](const AppState& state) {
                    return with_active_page(state, page);
                });
            })
            .build();
        current_y += ui::kNavItemHeight + ui::kNavItemGap;
    });
}

}  // namespace su::app::widgets