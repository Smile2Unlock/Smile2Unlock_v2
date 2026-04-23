module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>

export module su.app.ui.layout;

import std;
import su.app.presenters;
import su.app.state;
import su.app.ui.theme;
import su.app.widgets.content;
import su.app.widgets.header;
import su.app.widgets.sidebar;

export namespace su::app::ui {

using EUINEO::RectFrame;
using EUINEO::UIContext;

struct ShellFrames {
    RectFrame header;
    RectFrame sidebar;
    RectFrame content;
};

ShellFrames compute_shell_frames(const RectFrame& screen);
void render_app_layout(UIContext& ui, const RectFrame& screen, const AppState& state);

}  // namespace su::app::ui

namespace su::app::ui {

ShellFrames compute_shell_frames(const RectFrame& screen) {
    const auto header = RectFrame{
        kOuterMargin,
        kOuterMargin,
        screen.width - kOuterMargin * 2.0f,
        kHeaderHeight,
    };
    const auto sidebar = RectFrame{
        kOuterMargin,
        header.y + header.height + kSectionGap,
        std::min(kSidebarWidth, screen.width * 0.24f),
        screen.height - header.height - kOuterMargin * 2.0f - kSectionGap,
    };
    const auto content = RectFrame{
        sidebar.x + sidebar.width + kSectionGap,
        sidebar.y,
        std::max(320.0f, screen.width - sidebar.width - kOuterMargin * 2.0f - kSectionGap),
        sidebar.height,
    };
    return ShellFrames{
        .header = header,
        .sidebar = sidebar,
        .content = content,
    };
}

void render_app_layout(UIContext& ui, const RectFrame& screen, const AppState& state) {
    EUINEO::UseDslDarkTheme(stage_color());

    ui.panel("stage")
        .position(0.0f, 0.0f)
        .size(screen.width, screen.height)
        .background(stage_color())
        .gradient(EUINEO::RectGradient::Corners(
            stage_glow(),
            stage_color(),
            EUINEO::Color(0.07f, 0.09f, 0.12f, 1.0f),
            EUINEO::Color(0.12f, 0.15f, 0.21f, 1.0f)))
        .build();

    const auto frames = compute_shell_frames(screen);
    const auto view_model = presenters::make_shell_view_model(state, screen.width);
    widgets::render_header(ui, frames.header, view_model.header);
    widgets::render_sidebar(ui, frames.sidebar, view_model.sidebar);
    widgets::render_content(ui, frames.content, view_model.content);
}

}  // namespace su::app::ui
