module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>
#include <GLFW/glfw3.h>

export module su.app.ui.layout;

import std;
import su.app.presenters;
import su.app.state;
import su.app.ui.theme;

export namespace su::app::ui {

using EUINEO::RectFrame;
using EUINEO::UIContext;

struct ShellFrames {
    RectFrame title_bar;
    RectFrame sidebar;
    RectFrame content;
};

ShellFrames compute_shell_frames(const RectFrame& window);
void render_app_layout(UIContext& ui, const RectFrame& window, const AppState& state);

}  // namespace su::app::ui

namespace su::app::ui {

namespace {

GLFWwindow* get_glfw_window() {
    return EUINEO::ActiveDslWindowState().window;
}

void render_title_bar_impl(UIContext& ui, const RectFrame& frame);
void render_sidebar_impl(UIContext& ui, const RectFrame& frame, const AppState& state);
void render_content_area_impl(UIContext& ui, const RectFrame& frame, const AppState& state);
void render_page_cards_impl(UIContext& ui, const RectFrame& window_frame, float card_width, float card_height, float start_y);

}  // namespace

ShellFrames compute_shell_frames(const RectFrame& window) {
    return ShellFrames{
        .title_bar = make_title_bar_frame(window),
        .sidebar = make_sidebar_frame(window),
        .content = make_content_frame(window),
    };
}

void render_app_layout(UIContext& ui, const RectFrame& window, const AppState& state) {
    ui.panel("app.background")
        .position(0.0f, 0.0f)
        .size(window.width, window.height)
        .background(content_bg_color())
        .build();

    const auto frames = compute_shell_frames(window);
    render_title_bar_impl(ui, frames.title_bar);
    render_sidebar_impl(ui, frames.sidebar, state);
    render_content_area_impl(ui, frames.content, state);
}

namespace {

void render_title_bar_impl(UIContext& ui, const RectFrame& frame) {
    ui.panel("titlebar")
        .position(frame.x, frame.y)
        .size(frame.width, frame.height)
        .background(title_bar_color())
        .build();

    const auto close_btn_x = frame.x + frame.width - kWindowButtonSize - kWindowButtonGap;
    const auto btn_y = frame.y + (frame.height - kWindowButtonSize) / 2.0f;

    ui.panel("titlebar.drag")
        .position(frame.x, frame.y)
        .size(frame.width - 120.0f, frame.height)
        .background(title_bar_color())
        .build();
    ui.label("titlebar.title")
        .position(frame.x + 16.0f, frame.y + (frame.height - 20.0f) / 2.0f)
        .fontSize(13.0f)
        .color(text_secondary_color())
        .text("Smile2Unlock")
        .build();

    ui.button("titlebar.minimize")
        .position(close_btn_x - kWindowButtonSize * 2.0f - kWindowButtonGap * 2.0f, btn_y)
        .size(kWindowButtonSize, kWindowButtonSize)
        .style(EUINEO::ButtonStyle::Default)
        .onClick([]() {
            if (GLFWwindow* win = get_glfw_window()) {
                glfwIconifyWindow(win);
            }
        })
        .build();
    ui.button("titlebar.maximize")
        .position(close_btn_x - kWindowButtonSize - kWindowButtonGap, btn_y)
        .size(kWindowButtonSize, kWindowButtonSize)
        .style(EUINEO::ButtonStyle::Default)
        .onClick([]() {
            if (GLFWwindow* win = get_glfw_window()) {
                if (glfwGetWindowMonitor(win)) {
                    EUINEO::SetDslWindowFullscreen(false);
                } else {
                    EUINEO::SetDslWindowFullscreen(true);
                }
            }
        })
        .build();
    ui.button("titlebar.close")
        .position(close_btn_x, btn_y)
        .size(kWindowButtonSize, kWindowButtonSize)
        .style(EUINEO::ButtonStyle::Default)
        .onClick([]() {
            if (GLFWwindow* win = get_glfw_window()) {
                glfwSetWindowShouldClose(win, 1);
            }
        })
        .build();
}

void render_sidebar_impl(UIContext& ui, const RectFrame& frame, const AppState& state) {
    const auto sidebar_width = state.sidebar_collapsed ? kSidebarCollapsedWidth : kSidebarWidth;

    ui.panel("sidebar")
        .position(frame.x, frame.y)
        .size(sidebar_width, frame.height)
        .background(sidebar_color())
        .build();

    const auto toggle_x = frame.x + (sidebar_width - 32.0f) / 2.0f;
    const auto toggle_y = frame.y + 12.0f;
    ui.button("sidebar.toggle")
        .position(toggle_x, toggle_y)
        .size(32.0f, 32.0f)
        .style(EUINEO::ButtonStyle::Default)
        .onClick([]() {
            update_state([](const AppState& s) {
                return with_sidebar_collapsed(s, !s.sidebar_collapsed);
            });
        })
        .build();

    auto nav_y = frame.y + 60.0f;
    auto items = make_sidebar_nav_items(state);
    std::ranges::for_each(items, [&](const auto& item) {
        const auto item_width = sidebar_width - 16.0f;
        const auto item_x = frame.x + 8.0f;
        const bool is_active = state.active_page == item.page;

        if (state.sidebar_collapsed) {
            ui.button(item.id)
                .position(frame.x + (sidebar_width - 36.0f) / 2.0f, nav_y)
                .size(36.0f, 36.0f)
                .style(is_active ? EUINEO::ButtonStyle::Primary : EUINEO::ButtonStyle::Default)
                .onClick([page = item.page]() {
                    update_state([page](const AppState& s) {
                        return with_active_page(s, page);
                    });
                })
                .build();
        } else {
            ui.button(item.id)
                .position(item_x, nav_y)
                .size(item_width, kNavItemHeight)
                .text(item.label)
                .style(is_active ? EUINEO::ButtonStyle::Primary : EUINEO::ButtonStyle::Default)
                .onClick([page = item.page]() {
                    update_state([page](const AppState& s) {
                        return with_active_page(s, page);
                    });
                })
                .build();
        }
        nav_y += kNavItemHeight + kNavItemGap;
    });
}

void render_content_area_impl(UIContext& ui, const RectFrame& frame, const AppState& state) {
    ui.panel("content")
        .position(frame.x, frame.y)
        .size(frame.width, frame.height)
        .background(content_bg_color())
        .build();

    auto content_y = frame.y + kContentPadding;
    const auto card_width = (frame.width - kContentPadding * 2.0f - kCardGap) / 2.0f;
    const auto card_height = 120.0f;

    render_page_cards_impl(ui, frame, card_width, card_height, content_y);
}

void render_page_cards_impl(UIContext& ui, const RectFrame& window_frame, float card_width, float card_height, float start_y) {
    const auto card_x = window_frame.x + kContentPadding;

    ui.panel("card.dashboard")
        .position(card_x, start_y)
        .size(card_width, card_height)
        .background(card_bg_color())
        .rounding(12.0f)
        .border(1.0f, border_light_color())
        .build();
    ui.label("card.dashboard.title")
        .position(card_x + 16.0f, start_y + 16.0f)
        .fontSize(16.0f)
        .color(text_primary_color())
        .text("Dashboard")
        .build();
    ui.label("card.dashboard.body")
        .position(card_x + 16.0f, start_y + 44.0f)
        .fontSize(13.0f)
        .color(text_secondary_color())
        .text("System overview and quick actions")
        .build();

    const auto card2_x = card_x + card_width + kCardGap;
    ui.panel("card.enrollment")
        .position(card2_x, start_y)
        .size(card_width, card_height)
        .background(card_bg_color())
        .rounding(12.0f)
        .border(1.0f, border_light_color())
        .build();
    ui.label("card.enrollment.title")
        .position(card2_x + 16.0f, start_y + 16.0f)
        .fontSize(16.0f)
        .color(text_primary_color())
        .text("Enrollment")
        .build();
    ui.label("card.enrollment.body")
        .position(card2_x + 16.0f, start_y + 44.0f)
        .fontSize(13.0f)
        .color(text_secondary_color())
        .text("Manage users and face data")
        .build();

    const auto card3_y = start_y + card_height + kCardGap;
    ui.panel("card.recognition")
        .position(card_x, card3_y)
        .size(card_width, card_height)
        .background(card_bg_color())
        .rounding(12.0f)
        .border(1.0f, border_light_color())
        .build();
    ui.label("card.recognition.title")
        .position(card_x + 16.0f, card3_y + 16.0f)
        .fontSize(16.0f)
        .color(text_primary_color())
        .text("Recognition")
        .build();
    ui.label("card.recognition.body")
        .position(card_x + 16.0f, card3_y + 44.0f)
        .fontSize(13.0f)
        .color(text_secondary_color())
        .text("Start face recognition")
        .build();

    ui.panel("card.settings")
        .position(card2_x, card3_y)
        .size(card_width, card_height)
        .background(card_bg_color())
        .rounding(12.0f)
        .border(1.0f, border_light_color())
        .build();
    ui.label("card.settings.title")
        .position(card2_x + 16.0f, card3_y + 16.0f)
        .fontSize(16.0f)
        .color(text_primary_color())
        .text("Settings")
        .build();
    ui.label("card.settings.body")
        .position(card2_x + 16.0f, card3_y + 44.0f)
        .fontSize(13.0f)
        .color(text_secondary_color())
        .text("Configure application settings")
        .build();
}

}  // namespace

}  // namespace su::app::ui