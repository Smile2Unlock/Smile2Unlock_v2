module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>
#include <GLFW/glfw3.h>

export module su.app.ui.layout;

import std;
import su.app.i18n;
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

ShellFrames compute_shell_frames(const RectFrame& window, const AppState& state);
void render_app_layout(UIContext& ui, const RectFrame& window, const AppState& state);

}  // namespace su::app::ui

namespace su::app::ui {

namespace {

GLFWwindow* get_glfw_window() {
    return EUINEO::ActiveDslWindowState().window;
}

std::string tr(std::string_view locale, std::string_view key) {
    return i18n::text(i18n::app_i18n(), locale, key);
}

void render_title_bar_impl(UIContext& ui, const RectFrame& frame, const AppState& state);
void render_sidebar_impl(UIContext& ui, const RectFrame& frame, const AppState& state);
void render_content_area_impl(UIContext& ui, const RectFrame& frame, const AppState& state);
void render_page_interactive(UIContext& ui, const RectFrame& area, const AppState& state);
void render_dashboard_interactive(UIContext& ui, const RectFrame& area, std::string_view locale);
void render_enrollment_interactive(UIContext& ui, const RectFrame& area, std::string_view locale);
void render_recognition_interactive(UIContext& ui, const RectFrame& area, std::string_view locale);
void render_settings_interactive(UIContext& ui, const RectFrame& area, std::string_view locale);

constexpr std::string_view kIconSidebarToggle = "\xEF\x83\x89";      // fa-bars
constexpr std::string_view kWindowIconMinimizePath = "assets/icons/window_controls/minimize.svg";
constexpr std::string_view kWindowIconMaximizePath = "assets/icons/window_controls/maximize.svg";
constexpr std::string_view kWindowIconClosePath = "assets/icons/window_controls/close.svg";
constexpr float kWindowIconSize = 18.0f;

Color inverted_color(const Color& color, float alpha = 1.0f) {
    return Color(1.0f - color.r, 1.0f - color.g, 1.0f - color.b, alpha);
}

void render_window_control_icon(UIContext& ui,
                                std::string_view id,
                                float button_x,
                                float button_y,
                                std::string_view icon_path,
                                const Color& button_bg,
                                const Color& button_border,
                                const std::function<void()>& on_click) {
    const auto icon_x = button_x + (kWindowButtonSize - kWindowIconSize) * 0.5f;
    const auto icon_y = button_y + (kWindowButtonSize - kWindowIconSize) * 0.5f;

    ui.panel(std::format("{}.bg", id))
        .position(button_x, button_y)
        .size(kWindowButtonSize, kWindowButtonSize)
        .background(button_bg)
        .rounding(8.0f)
        .border(1.0f, button_border)
        .build();

    ui.image(std::format("{}.icon", id))
        .position(icon_x, icon_y)
        .size(kWindowIconSize, kWindowIconSize)
        .path(std::string(icon_path))
        .build();

    ui.button(std::string(id))
        .position(button_x, button_y)
        .size(kWindowButtonSize, kWindowButtonSize)
        .opacity(0.0f)
        .onClick(on_click)
        .build();
}

}  // namespace

ShellFrames compute_shell_frames(const RectFrame& window, const AppState& state) {
    return ShellFrames{
        .title_bar = make_title_bar_frame(window),
        .sidebar = make_sidebar_frame(window, state.sidebar_collapsed),
        .content = make_content_frame(window, state.sidebar_collapsed),
    };
}

void render_app_layout(UIContext& ui, const RectFrame& window, const AppState& state) {
    ui.panel("app.background")
        .position(0.0f, 0.0f)
        .size(window.width, window.height)
        .background(content_bg_color())
        .build();

    const auto frames = compute_shell_frames(window, state);
    render_title_bar_impl(ui, frames.title_bar, state);
    render_sidebar_impl(ui, frames.sidebar, state);
    render_content_area_impl(ui, frames.content, state);
}

namespace {

void render_title_bar_impl(UIContext& ui, const RectFrame& frame, const AppState& state) {
    (void)state;
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

    constexpr float title_font = 15.0f;
    ui.label("titlebar.title")
        .position(frame.x + 16.0f, frame.y + (frame.height - title_font) * 0.5f)
        .fontSize(title_font)
        .color(text_secondary_color())
        .text(tr(state.active_locale, "titlebar.title"))
        .build();

    const auto neutral_button_bg = inverted_color(title_bar_color(), 0.22f);
    const auto neutral_button_border = inverted_color(title_bar_color(), 0.34f);
    const auto close_button_bg = Color(0.34f, 0.36f, 0.40f, 0.92f);
    const auto close_button_border = Color(1.0f, 1.0f, 1.0f, 0.14f);
    const auto minimize_x = close_btn_x - kWindowButtonSize * 2.0f - kWindowButtonGap * 2.0f;
    render_window_control_icon(ui,
                               "titlebar.minimize",
                               minimize_x,
                               btn_y,
                               kWindowIconMinimizePath,
                               neutral_button_bg,
                               neutral_button_border,
                               []() {
        if (GLFWwindow* win = get_glfw_window()) {
            glfwIconifyWindow(win);
        }
    });

    const auto maximize_x = close_btn_x - kWindowButtonSize - kWindowButtonGap;
    render_window_control_icon(ui,
                               "titlebar.maximize",
                               maximize_x,
                               btn_y,
                               kWindowIconMaximizePath,
                               neutral_button_bg,
                               neutral_button_border,
                               []() {
        if (GLFWwindow* win = get_glfw_window()) {
            if (glfwGetWindowAttrib(win, GLFW_MAXIMIZED) == GLFW_TRUE) {
                glfwRestoreWindow(win);
            } else {
                glfwMaximizeWindow(win);
            }
        }
    });

    render_window_control_icon(ui,
                               "titlebar.close",
                               close_btn_x,
                               btn_y,
                               kWindowIconClosePath,
                               close_button_bg,
                               close_button_border,
                               []() {
        if (GLFWwindow* win = get_glfw_window()) {
            glfwSetWindowShouldClose(win, 1);
        }
    });
}

void render_sidebar_impl(UIContext& ui, const RectFrame& frame, const AppState& state) {
    constexpr float kSidebarHeaderPadding = 12.0f;
    constexpr float kCollapsedNavButtonSize = 34.0f;

    ui.panel("sidebar")
        .position(frame.x, frame.y)
        .size(frame.width, frame.height)
        .background(sidebar_color())
        .build();

    const auto toggle_x = frame.x + kSidebarHeaderPadding;
    const auto toggle_y = frame.y + kSidebarHeaderPadding;
    ui.button("sidebar.toggle")
        .position(toggle_x, toggle_y)
        .size(kSidebarToggleSize, kSidebarToggleSize)
        .style(EUINEO::ButtonStyle::Default)
        .icon(std::string(kIconSidebarToggle))
        .fontSize(15.0f)
        .onClick([]() {
            update_state([](const AppState& s) {
                return with_sidebar_collapsed(s, !s.sidebar_collapsed);
            });
        })
        .build();

    auto nav_y = toggle_y + kSidebarToggleSize + 14.0f;
    auto items = make_sidebar_nav_items(state);
    std::ranges::for_each(items, [&](const auto& item) {
        const auto item_width = frame.width - 16.0f;
        const auto item_x = frame.x + 8.0f;
        const bool is_active = state.active_page == item.page;

        if (state.sidebar_collapsed) {
            ui.button(item.id)
                .position(frame.x + (frame.width - kCollapsedNavButtonSize) * 0.5f, nav_y)
                .size(kCollapsedNavButtonSize, kCollapsedNavButtonSize)
                .icon(item.icon)
                .fontSize(15.0f)
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
                .icon(item.icon)
                .fontSize(15.0f)
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

    const auto& locale = state.active_locale;
    const auto page_str = page_name(state.active_page);
    auto current_y = frame.y + kContentPadding;

    const auto title_key = std::format("page.{}.title", page_str);
    const auto desc_key = std::format("page.{}.description", page_str);

    ui.label("content.page.title")
        .position(frame.x + kContentPadding, current_y)
        .fontSize(22.0f)
        .color(text_primary_color())
        .text(tr(locale, title_key))
        .build();
    current_y += 30.0f;

    ui.label("content.page.description")
        .position(frame.x + kContentPadding, current_y)
        .fontSize(14.0f)
        .color(text_secondary_color())
        .text(tr(locale, desc_key))
        .build();
    current_y += 50.0f;

    const auto interactive_h = frame.y + frame.height - current_y - kContentPadding;
    const RectFrame interactive_area{frame.x + kContentPadding, current_y, frame.width - kContentPadding * 2.0f, interactive_h};

    render_page_interactive(ui, interactive_area, state);
}

void render_page_interactive(UIContext& ui, const RectFrame& area, const AppState& state) {
    using RenderFn = void (*)(UIContext&, const RectFrame&, std::string_view);
    static const RenderFn renderers[] = {
        [](UIContext& u, const RectFrame& a, std::string_view l) { render_dashboard_interactive(u, a, l); },
        [](UIContext& u, const RectFrame& a, std::string_view l) { render_enrollment_interactive(u, a, l); },
        [](UIContext& u, const RectFrame& a, std::string_view l) { render_recognition_interactive(u, a, l); },
        [](UIContext& u, const RectFrame& a, std::string_view l) { render_settings_interactive(u, a, l); },
    };

    const auto idx = static_cast<std::size_t>(state.active_page);
    if (idx < std::size(renderers)) {
        renderers[idx](ui, area, state.active_locale);
    }
}

void render_dashboard_interactive(UIContext& ui, const RectFrame& area, std::string_view) {
    const auto half_w = (area.width - kCardGap) * 0.5f;
    const auto third_h = (area.height - kCardGap * 2.0f) / 3.0f;

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 2; ++col) {
            const auto x = area.x + col * (half_w + kCardGap);
            const auto y = area.y + row * (third_h + kCardGap);
            ui.panel(std::format("dashboard.cell.{}.{}", row, col))
                .position(x, y)
                .size(half_w, third_h)
                .background(card_bg_color())
                .rounding(12.0f)
                .build();
        }
    }
}

void render_enrollment_interactive(UIContext& ui, const RectFrame& area, std::string_view) {
    const auto left_w = area.width * 0.55f;
    const auto right_w = area.width - left_w - kCardGap;

    ui.panel("enrollment.form")
        .position(area.x, area.y)
        .size(left_w, area.height)
        .background(card_bg_color())
        .rounding(12.0f)
        .build();

    for (int i = 0; i < 4; ++i) {
        const auto fy = area.y + 20.0f + i * 56.0f;
        ui.panel(std::format("enrollment.field.{}", i))
            .position(area.x + 20.0f, fy)
            .size(left_w - 40.0f, 40.0f)
            .background(content_bg_color())
            .rounding(8.0f)
            .build();
    }

    ui.panel("enrollment.preview")
        .position(area.x + left_w + kCardGap, area.y)
        .size(right_w, area.height)
        .background(card_bg_color())
        .rounding(12.0f)
        .build();
}

void render_recognition_interactive(UIContext& ui, const RectFrame& area, std::string_view) {
    const auto ctrl_h = 56.0f;

    ui.panel("recognition.viewport")
        .position(area.x, area.y)
        .size(area.width, area.height - ctrl_h - kCardGap)
        .background(card_bg_color())
        .rounding(12.0f)
        .build();

    ui.panel("recognition.controls")
        .position(area.x, area.y + area.height - ctrl_h)
        .size(area.width, ctrl_h)
        .background(card_bg_color())
        .rounding(12.0f)
        .build();

    for (int i = 0; i < 3; ++i) {
        const auto bx = area.x + 16.0f + i * 80.0f;
        ui.panel(std::format("recognition.btn.{}", i))
            .position(bx, area.y + area.height - ctrl_h + 8.0f)
            .size(64.0f, 40.0f)
            .background(content_bg_color())
            .rounding(8.0f)
            .build();
    }
}

void render_settings_interactive(UIContext& ui, const RectFrame& area, std::string_view) {
    const auto row_h = 52.0f;

    for (int i = 0; i < 5; ++i) {
        const auto y = area.y + i * (row_h + kCardGap);
        ui.panel(std::format("settings.row.{}", i))
            .position(area.x, y)
            .size(area.width, row_h)
            .background(card_bg_color())
            .rounding(10.0f)
            .build();
    }
}

}  // namespace

}  // namespace su::app::ui
