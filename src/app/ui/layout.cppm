module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>
#include <GLFW/glfw3.h>

export module su.app.ui.layout;

import std;
import su.app.i18n;
import su.app.state;
import su.app.ui.theme;
import su.app.services.backend;

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

using EUINEO::Color;
using EUINEO::ButtonStyle;

GLFWwindow* get_glfw_window() {
    return EUINEO::ActiveDslWindowState().window;
}

std::string tr(std::string_view locale, std::string_view key) {
    return i18n::text(i18n::app_i18n(), locale, key);
}

std::string trf(std::string_view locale, std::string_view key, auto&&... args) {
    return i18n::format(i18n::app_i18n(), locale, key, std::forward<decltype(args)>(args)...);
}

float baseline_for_top(std::string_view text, float font_size, float top_y) {
    const auto bounds = EUINEO::Renderer::MeasureTextBounds(std::string(text), font_size / 24.0f);
    return top_y - bounds.y;
}

float baseline_for_center(std::string_view text, float font_size, float center_y) {
    const auto bounds = EUINEO::Renderer::MeasureTextBounds(std::string(text), font_size / 24.0f);
    return center_y - (bounds.y + bounds.height * 0.5f);
}

// --- Forward declarations ---
void render_title_bar_impl(UIContext& ui, const RectFrame& frame, const AppState& state);
void render_sidebar_impl(UIContext& ui, const RectFrame& frame, const AppState& state);
void render_content_area_impl(UIContext& ui, const RectFrame& frame, const AppState& state);
void render_page_interactive(UIContext& ui, const RectFrame& area, const AppState& state);
void render_dashboard_interactive(UIContext& ui, const RectFrame& area, const AppState& state);
void render_enrollment_interactive(UIContext& ui, const RectFrame& area, const AppState& state);
void render_recognition_interactive(UIContext& ui, const RectFrame& area, const AppState& state);
void render_settings_interactive(UIContext& ui, const RectFrame& area, const AppState& state);

constexpr std::string_view kIconSidebarToggle = "\xEF\x83\x89";      // fa-bars
constexpr std::string_view kWindowIconMinimizePath = "assets/icons/window_controls/minimize.svg";
constexpr std::string_view kWindowIconMaximizePath = "assets/icons/window_controls/maximize.svg";
constexpr std::string_view kWindowIconClosePath = "assets/icons/window_controls/close.svg";
constexpr float kWindowIconSize = 18.0f;
constexpr float kFontPageTitle = 34.0f;
constexpr float kFontPageSubtitle = 16.0f;
constexpr float kFontSectionTitle = 18.0f;
constexpr float kFontBodyStrong = 16.0f;
constexpr float kFontBody = 15.0f;
constexpr float kFontMeta = 14.0f;
constexpr float kFontButton = 15.0f;
constexpr float kFontCardValue = 40.0f;

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

// --- Stat card widget ---
// Compact card with icon + value on the same area, title below value.
// Uses MeasureTextBounds for proper vertical centering.
void render_stat_card(UIContext& ui,
                      const std::string& id,
                      float x, float y, float w, float h,
                      std::string_view title,
                      std::string_view value,
                      std::string_view icon,
                      const Color& accent_color) {
    ui.panel(std::format("{}.bg", id))
        .position(x, y)
        .size(w, h)
        .background(card_bg_color())
        .rounding(12.0f)
        .border(1.0f, border_light_color())
        .build();

    constexpr float kIconFontSize = 22.0f;
    constexpr float kIconBadgeSize = 34.0f;
    constexpr float kContentInset = 24.0f;
    const auto icon_badge_x = x + kContentInset;
    const auto icon_badge_y = y + 18.0f;
    ui.panel(std::format("{}.icon_badge", id))
        .position(icon_badge_x, icon_badge_y)
        .size(kIconBadgeSize, kIconBadgeSize)
        .background(Color(accent_color.r, accent_color.g, accent_color.b, 0.14f))
        .rounding(10.0f)
        .build();

    const auto icon_x = icon_badge_x + (kIconBadgeSize - kIconFontSize) * 0.5f;
    const auto icon_y = baseline_for_center(icon, kIconFontSize, icon_badge_y + kIconBadgeSize * 0.5f);
    ui.label(std::format("{}.icon", id))
        .position(icon_x, icon_y)
        .fontSize(kIconFontSize)
        .color(accent_color)
        .text(std::string(icon))
        .build();

    const float value_font_size = value.size() > 5 ? 30.0f : kFontCardValue;
    const auto val_value = std::string(value);
    const float val_y = baseline_for_top(val_value, value_font_size, y + h - 86.0f);
    ui.label(std::format("{}.value", id))
        .position(x + kContentInset, val_y)
        .fontSize(value_font_size)
        .color(text_primary_color())
        .text(val_value)
        .build();

    constexpr float kTitleFontSize = kFontBodyStrong;
    const auto title_str = std::string(title);
    const float title_y = baseline_for_top(title_str, kTitleFontSize, y + h - 38.0f);
    ui.label(std::format("{}.title", id))
        .position(x + kContentInset, title_y)
        .fontSize(kTitleFontSize)
        .color(text_secondary_color())
        .text(title_str)
        .build();
}

}  // namespace (anonymous)

// ============================================================================
// Shell frame computation
// ============================================================================

ShellFrames compute_shell_frames(const RectFrame& window, const AppState& state) {
    return ShellFrames{
        .title_bar = make_title_bar_frame(window),
        .sidebar = make_sidebar_frame(window, state.sidebar_collapsed),
        .content = make_content_frame(window, state.sidebar_collapsed),
    };
}

// ============================================================================
// Main app layout
// ============================================================================

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

// ============================================================================
// Title bar
// ============================================================================

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

    constexpr float title_font = kFontSectionTitle;
    const auto title_bounds = EUINEO::Renderer::MeasureTextBounds(
        tr(state.active_locale, "titlebar.title"), title_font / 24.0f);
    const auto title_center_y = btn_y + kWindowButtonSize * 0.5f;
    const auto title_y = title_center_y - (title_bounds.y + title_bounds.height * 0.5f);
    ui.label("titlebar.title")
        .position(frame.x + 16.0f, title_y)
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

}  // namespace (anonymous)

// ============================================================================
// Sidebar
// ============================================================================

namespace {

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
        .fontSize(kFontBody)
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
                .fontSize(kFontBody)
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
                .fontSize(kFontBody)
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

}  // namespace (anonymous)

// ============================================================================
// Content area shell
// ============================================================================

namespace {

void render_content_area_impl(UIContext& ui, const RectFrame& frame, const AppState& state) {
    constexpr float kContentTitleFont = kFontPageTitle;
    constexpr float kContentDescriptionFont = kFontPageSubtitle;
    constexpr float kContentHeaderTop = 12.0f;
    constexpr float kContentHeaderGap = 10.0f;

    ui.panel("content")
        .position(frame.x, frame.y)
        .size(frame.width, frame.height)
        .background(content_bg_color())
        .build();

    const auto& locale = state.active_locale;
    const auto page_str = page_name(state.active_page);
    auto current_y = frame.y + kContentHeaderTop;

    const auto title_key = std::format("page.{}.title", page_str);
    const auto desc_key = std::format("page.{}.description", page_str);
    const auto title_text = tr(locale, title_key);
    const auto desc_text = tr(locale, desc_key);
    const auto title_bounds = EUINEO::Renderer::MeasureTextBounds(title_text, kContentTitleFont / 24.0f);
    const auto desc_bounds = EUINEO::Renderer::MeasureTextBounds(desc_text, kContentDescriptionFont / 24.0f);
    const auto title_top = frame.y + kContentHeaderTop;
    const auto title_y = title_top - title_bounds.y;

    ui.label("content.page.title")
        .position(frame.x + kContentPadding, title_y)
        .fontSize(kContentTitleFont)
        .color(text_primary_color())
        .text(title_text)
        .build();
    const auto title_bottom = title_y + title_bounds.y + title_bounds.height;
    const auto desc_top = title_bottom + kContentHeaderGap;
    const auto desc_y = desc_top - desc_bounds.y;

    ui.label("content.page.description")
        .position(frame.x + kContentPadding, desc_y)
        .fontSize(kContentDescriptionFont)
        .color(text_secondary_color())
        .text(desc_text)
        .build();
    current_y = desc_y + desc_bounds.y + desc_bounds.height + 24.0f;

    const auto interactive_h = frame.y + frame.height - current_y - kContentPadding;
    const RectFrame interactive_area{
        frame.x + kContentPadding, current_y,
        frame.width - kContentPadding * 2.0f, interactive_h};

    render_page_interactive(ui, interactive_area, state);
}

}  // namespace (anonymous)

// ============================================================================
// Page dispatcher
// ============================================================================

namespace {

void render_page_interactive(UIContext& ui, const RectFrame& area, const AppState& state) {
    switch (state.active_page) {
        case PageId::Dashboard:
            render_dashboard_interactive(ui, area, state);
            break;
        case PageId::Enrollment:
            render_enrollment_interactive(ui, area, state);
            break;
        case PageId::Recognition:
            render_recognition_interactive(ui, area, state);
            break;
        case PageId::Settings:
            render_settings_interactive(ui, area, state);
            break;
    }
}

}  // namespace (anonymous)

// ============================================================================
// DASHBOARD PAGE — Status overview with metric cards + quick actions
// ============================================================================

namespace {

void render_dashboard_interactive(UIContext& ui, const RectFrame& area, const AppState& state) {
    const auto card_w = (area.width - kCardGap) * 0.5f;
    constexpr float kDashboardActionBarHeight = 78.0f;
    const auto card_h = (area.height - kDashboardActionBarHeight - kCardGap * 2.0f) * 0.5f;

    // Row 0: System Status + Users
    const auto status_color = state.camera_connected ? accent_success_color() : accent_warning_color();
    render_stat_card(ui, "dash.status", area.x, area.y, card_w, card_h,
                     tr(state.active_locale, "card.dashboard.status_title"),
                     tr(state.active_locale, state.camera_connected
                         ? "card.dashboard.status_ok" : "card.dashboard.status_warn"),
                     "\xEF\x84\x9F", status_color);  // fa-check-circle

    const auto users_value = std::format("{}", state.total_users);
    render_stat_card(ui, "dash.users", area.x + card_w + kCardGap, area.y, card_w, card_h,
                     tr(state.active_locale, "card.dashboard.users_title"),
                     users_value,
                     "\xEF\x80\x87", accent_primary_color());  // fa-users

    // Row 1: Faces + Camera
    const auto faces_value = std::format("{}", state.total_faces);
    render_stat_card(ui, "dash.faces", area.x, area.y + card_h + kCardGap, card_w, card_h,
                     tr(state.active_locale, "card.dashboard.faces_title"),
                     faces_value,
                     "\xEF\x80\xB0", accent_success_color());  // fa-camera

    const auto cam_status = state.camera_connected
        ? tr(state.active_locale, "card.dashboard.camera_connected")
        : tr(state.active_locale, "card.dashboard.camera_disconnected");
    render_stat_card(ui, "dash.camera", area.x + card_w + kCardGap, area.y + card_h + kCardGap, card_w, card_h,
                     tr(state.active_locale, "card.dashboard.camera_title"),
                     cam_status,
                     "\xEF\x81\xA0", accent_primary_color());  // fa-video

    // Bottom row: Recognition status + quick action buttons
    const auto bottom_y = area.y + (card_h + kCardGap) * 2.0f;
    const auto bottom_h = kDashboardActionBarHeight;

    ui.panel("dash.actions.bg")
        .position(area.x, bottom_y)
        .size(area.width, bottom_h)
        .background(card_bg_color())
        .rounding(12.0f)
        .border(1.0f, border_light_color())
        .build();

    constexpr float kActionGap = 12.0f;
    constexpr float kActionBtnW = 124.0f;
    constexpr float kActionBtnH = 32.0f;
    const float action_center_y = bottom_y + bottom_h * 0.5f;
    const float btn_y = action_center_y - kActionBtnH * 0.5f;

    // Recognition status badge (left side)
    const auto rec_value = state.recognition_running
        ? tr(state.active_locale, "card.dashboard.rec_running")
        : tr(state.active_locale, "card.dashboard.rec_idle");
    const auto rec_color = state.recognition_running ? accent_success_color() : text_muted_color();
    constexpr float kStatusFontSize = 15.0f;
    const auto status_text = std::format("{}  {}",
        state.recognition_running ? "\xEF\x82\xB1" : "\xEF\x81\xB3", rec_value);
    const float status_y = baseline_for_center(status_text, kStatusFontSize, action_center_y);
    ui.label("dash.rec_status")
        .position(area.x + 18.0f, status_y)
        .fontSize(kStatusFontSize)
        .color(rec_color)
        .text(status_text)
        .build();

    // Action buttons (right side)
    const float btns_right = area.x + area.width - 18.0f;
    const float btn3_x = btns_right - kActionBtnW;
    const float btn2_x = btn3_x - kActionBtnW - kActionGap;
    const float btn1_x = btn2_x - kActionBtnW - kActionGap;

    // Enrollment button
    ui.button("dash.action.enroll")
        .position(btn1_x, btn_y)
        .size(kActionBtnW, kActionBtnH)
        .style(ButtonStyle::Primary)
        .text(tr(state.active_locale, "card.dashboard.btn_enroll"))
        .fontSize(kFontMeta)
        .onClick([]() {
            update_state([](const AppState& s) {
                return with_active_page(s, PageId::Enrollment);
            });
        })
        .build();

    // Recognition button
    ui.button("dash.action.recognize")
        .position(btn2_x, btn_y)
        .size(kActionBtnW, kActionBtnH)
        .style(ButtonStyle::Outline)
        .text(tr(state.active_locale, "card.dashboard.btn_recognize"))
        .fontSize(kFontMeta)
        .onClick([]() {
            update_state([](const AppState& s) {
                return with_active_page(s, PageId::Recognition);
            });
        })
        .build();

    // Settings button
    ui.button("dash.action.settings")
        .position(btn3_x, btn_y)
        .size(kActionBtnW, kActionBtnH)
        .style(ButtonStyle::Default)
        .text(tr(state.active_locale, "card.dashboard.btn_settings"))
        .fontSize(kFontMeta)
        .onClick([]() {
            update_state([](const AppState& s) {
                return with_active_page(s, PageId::Settings);
            });
        })
        .build();
}

}  // namespace (anonymous)

// ============================================================================
// ENROLLMENT PAGE — User list + add/edit + face enrollment
// ============================================================================

namespace {

void render_enrollment_interactive(UIContext& ui, const RectFrame& area, const AppState& state) {
    const auto& locale = state.active_locale;
    const auto left_w = area.width * 0.38f;
    const auto right_w = area.width - left_w - kCardGap;
    constexpr float kEnrollPanelTitleFont = kFontSectionTitle;
    constexpr float kEnrollItemTitleFont = kFontBodyStrong;
    constexpr float kEnrollItemMetaFont = kFontMeta;

    // ====== Left panel: User list ======
    ui.panel("enroll.list.bg")
        .position(area.x, area.y)
        .size(left_w, area.height)
        .background(card_bg_color())
        .rounding(12.0f)
        .border(1.0f, border_light_color())
        .build();

    ui.label("enroll.list.title")
        .position(area.x + 16.0f, area.y + 14.0f)
        .fontSize(kEnrollPanelTitleFont)
        .color(text_primary_color())
        .text(tr(locale, "enrollment.user_list"))
        .build();

    // User list items (scrollable via clipping)
    const auto list_top = area.y + 74.0f;
    const auto list_bottom = area.y + area.height - 66.0f;
    const auto list_h = list_bottom - list_top;
    const auto list_item_h = 52.0f;

    // Clip user list rendering
    const int max_visible = static_cast<int>(list_h / list_item_h);
    const auto& users = state.users;

    std::ranges::for_each(
        std::views::iota(0, std::min(max_visible, static_cast<int>(users.size()))),
        [&](int i) {
            const auto& user = users[i];
            const auto item_y = list_top + i * list_item_h;
            const bool selected = user.user_id == state.selected_user_id;
            const auto item_bg = selected ? sidebar_active_color() : Color(0.0f, 0.0f, 0.0f, 0.0f);

            ui.panel(std::format("enroll.list.item.{}", user.user_id))
                .position(area.x + 8.0f, item_y)
                .size(left_w - 16.0f, list_item_h - 4.0f)
                .background(item_bg)
                .rounding(8.0f)
                .build();

            ui.label(std::format("enroll.list.name.{}", user.user_id))
                .position(area.x + 18.0f, item_y + 10.0f)
                .fontSize(kEnrollItemTitleFont)
                .color(text_primary_color())
                .text(user.username)
                .build();

            const auto face_info = std::format("{} {}", user.face_count,
                tr(locale, "enrollment.faces_count"));
            ui.label(std::format("enroll.list.detail.{}", user.user_id))
                .position(area.x + 18.0f, item_y + 31.0f)
                .fontSize(kEnrollItemMetaFont)
                .color(text_muted_color())
                .text(face_info)
                .build();

            // Clickable overlay
            ui.button(std::format("enroll.list.btn.{}", user.user_id))
                .position(area.x + 8.0f, item_y)
                .size(left_w - 16.0f, list_item_h - 4.0f)
                .opacity(0.0f)
                .onClick([uid = user.user_id]() {
                    update_state([uid](const AppState& s) {
                        return with_selected_user(s, uid);
                    });
                })
                .build();
        });

    // Add user button
    ui.button("enroll.add_user")
        .position(area.x + 8.0f, list_bottom + 4.0f)
        .size(left_w - 16.0f, 40.0f)
        .style(ButtonStyle::Primary)
        .icon("\xEF\x81\xA7")  // fa-plus
        .text(tr(locale, "enrollment.add_user"))
        .fontSize(kFontButton)
        .onClick([]() {
            update_state([](const AppState& s) {
                return with_show_add_user(s, !s.show_add_user);
            });
        })
        .build();

    // ====== Right panel: User detail / Add form / Face enrollment ======
    const auto right_x = area.x + left_w + kCardGap;
    const auto right_inner_w = right_w - 32.0f;

    ui.panel("enroll.detail.bg")
        .position(right_x, area.y)
        .size(right_w, area.height)
        .background(card_bg_color())
        .rounding(12.0f)
        .border(1.0f, border_light_color())
        .build();

    if (state.show_add_user) {
        // Add user form
        ui.label("enroll.form.title")
            .position(right_x + 16.0f, area.y + 16.0f)
            .fontSize(kFontSectionTitle)
            .color(text_primary_color())
            .text(tr(locale, "enrollment.add_user_title"))
            .build();

        ui.input("enroll.form.username")
            .position(right_x + 16.0f, area.y + 52.0f)
            .size(right_inner_w, 36.0f)
            .placeholder(tr(locale, "enrollment.username_placeholder"))
            .fontSize(kFontBody)
            .build();

        ui.input("enroll.form.remark")
            .position(right_x + 16.0f, area.y + 96.0f)
            .size(right_inner_w, 36.0f)
            .placeholder(tr(locale, "enrollment.remark_placeholder"))
            .fontSize(kFontBody)
            .build();

        ui.button("enroll.form.save")
            .position(right_x + 16.0f, area.y + 148.0f)
            .size((right_inner_w - 8.0f) * 0.5f, 36.0f)
            .style(ButtonStyle::Primary)
            .text(tr(locale, "enrollment.save"))
            .fontSize(kFontButton)
            .onClick([]() {
                update_state([](const AppState& s) {
                    return with_show_add_user(s, false);
                });
            })
            .build();

        ui.button("enroll.form.cancel")
            .position(right_x + 16.0f + (right_inner_w - 8.0f) * 0.5f + 16.0f, area.y + 148.0f)
            .size((right_inner_w - 8.0f) * 0.5f, 36.0f)
            .style(ButtonStyle::Default)
            .text(tr(locale, "enrollment.cancel"))
            .fontSize(kFontButton)
            .onClick([]() {
                update_state([](const AppState& s) {
                    return with_show_add_user(s, false);
                });
            })
            .build();
    } else if (state.selected_user_id > 0) {
        // Selected user detail
        const auto sel = std::ranges::find_if(users, [&](const auto& u) {
            return u.user_id == state.selected_user_id;
        });

        if (sel != users.end()) {
            ui.label("enroll.detail.title")
                .position(right_x + 16.0f, area.y + 16.0f)
                .fontSize(22.0f)
                .color(text_primary_color())
                .text(sel->username)
                .build();

            ui.label("enroll.detail.remark")
                .position(right_x + 16.0f, area.y + 46.0f)
                .fontSize(kFontMeta)
                .color(text_secondary_color())
                .text(std::format("{}: {}", tr(locale, "enrollment.remark"), sel->remark))
                .build();

            ui.label("enroll.detail.faces")
                .position(right_x + 16.0f, area.y + 68.0f)
                .fontSize(kFontMeta)
                .color(text_secondary_color())
                .text(std::format("{}: {} {}",
                    tr(locale, "enrollment.faces"),
                    sel->face_count,
                    tr(locale, "enrollment.faces_count")))
                .build();

            // Face enrollment controls
            ui.panel("enroll.face.preview")
                .position(right_x + 16.0f, area.y + 100.0f)
                .size(right_inner_w, 200.0f)
                .background(content_bg_color())
                .rounding(10.0f)
                .border(1.0f, border_light_color())
                .build();

            ui.label("enroll.face.placeholder")
                .position(right_x + 16.0f + (right_inner_w - 120.0f) * 0.5f, area.y + 100.0f + 100.0f - 8.0f)
                .fontSize(kFontBody)
                .color(text_muted_color())
                .text(state.is_capturing_face
                    ? tr(locale, "enrollment.capturing")
                    : tr(locale, "enrollment.no_face"))
                .build();

            const auto btn_y = area.y + 320.0f;

            ui.button("enroll.face.capture")
                .position(right_x + 16.0f, btn_y)
                .size((right_inner_w - 8.0f) * 0.5f, 36.0f)
                .style(ButtonStyle::Primary)
                .text(tr(locale, "enrollment.capture_face"))
                .fontSize(kFontButton)
                .icon("\xEF\x80\xB0")
                .onClick([uid = sel->user_id]() {
                    update_state([uid](const AppState& s) {
                        auto next = with_capturing_face(s, true);
                        services::backend().capture_and_add_face(uid);
                        return with_capturing_face(next, false);
                    });
                })
                .build();

            ui.button("enroll.face.delete_user")
                .position(right_x + 16.0f + (right_inner_w - 8.0f) * 0.5f + 16.0f, btn_y)
                .size((right_inner_w - 8.0f) * 0.5f, 36.0f)
                .style(ButtonStyle::Default)
                .text(tr(locale, "enrollment.delete_user"))
                .fontSize(kFontButton)
                .onClick([uid = sel->user_id]() {
                    services::backend().delete_user(uid);
                    update_state([](const AppState& s) {
                        auto remaining = s.users;
                        std::erase_if(remaining, [&](const auto& u) {
                            return u.user_id == s.selected_user_id;
                        });
                        return with_users(with_selected_user(s, -1), std::move(remaining));
                    });
                })
                .build();
        }
    } else {
        // No user selected
        ui.label("enroll.no_selection")
            .position(right_x + 20.0f, area.y + 32.0f)
            .fontSize(kFontSectionTitle)
            .color(text_muted_color())
            .text(tr(locale, "enrollment.select_user_hint"))
            .build();
    }
}

}  // namespace (anonymous)

// ============================================================================
// RECOGNITION PAGE — Camera viewport + controls + result display
// ============================================================================

namespace {

void render_recognition_interactive(UIContext& ui, const RectFrame& area, const AppState& state) {
    const auto& locale = state.active_locale;
    const auto ctrl_h = 48.0f;

    // ====== Viewport area (camera preview placeholder) ======
    const auto view_h = area.height - ctrl_h - kCardGap;
    ui.panel("recog.viewport.bg")
        .position(area.x, area.y)
        .size(area.width, view_h)
        .background(card_bg_color())
        .rounding(12.0f)
        .border(1.0f, border_light_color())
        .build();

    // Viewport center status
    const auto view_center_x = area.x + area.width * 0.5f;
    const auto view_center_y = area.y + view_h * 0.5f;

    if (state.recognition_running) {
        // Running indicator
        ui.label("recog.viewport.status")
            .position(view_center_x - 90.0f, view_center_y - 28.0f)
            .fontSize(kFontSectionTitle)
            .color(accent_success_color())
            .text(std::format("\xEF\x80\xB0  {}", tr(locale, "recognition.running")))
            .build();

        ui.label("recog.viewport.fps")
            .position(view_center_x - 80.0f, view_center_y - 10.0f)
            .fontSize(kFontMeta)
            .color(text_secondary_color())
            .text(std::format("{}", tr(locale, "recognition.fps_placeholder")))
            .build();
    } else {
        ui.label("recog.viewport.idle")
            .position(view_center_x - 84.0f, view_center_y - 8.0f)
            .fontSize(kFontBodyStrong)
            .color(text_muted_color())
            .text(std::format("\xEF\x81\xB3  {}", tr(locale, "recognition.idle")))
            .build();
    }

    // Camera index indicator
    ui.label("recog.viewport.camera_info")
        .position(area.x + 14.0f, area.y + 14.0f)
        .fontSize(kFontBody)
        .color(text_muted_color())
        .text(std::format("{}: #{}", tr(locale, "recognition.camera_label"),
            state.config.selected_camera))
        .build();

    // ====== Last result overlay ======
    if (state.last_result.success) {
        const auto result_y = area.y + view_h - 48.0f;
        ui.panel("recog.result.banner")
            .position(area.x + 4.0f, result_y)
            .size(area.width - 8.0f, 44.0f)
            .background(state.last_result.success
                ? Color(0.20f, 0.55f, 0.30f, 0.85f)
                : Color(0.55f, 0.20f, 0.25f, 0.85f))
            .rounding(8.0f)
            .build();

        ui.label("recog.result.text")
            .position(area.x + 16.0f, result_y + 10.0f)
            .fontSize(kFontMeta)
            .color(text_primary_color())
            .text(std::format("{}{}: {:.1f}%",
                state.last_result.username,
                tr(locale, "recognition.matched"),
                state.last_result.similarity * 100.0f))
            .build();
    }

    // ====== Controls bar ======
    const auto ctrl_y = area.y + view_h + kCardGap;
    ui.panel("recog.controls.bg")
        .position(area.x, ctrl_y)
        .size(area.width, ctrl_h)
        .background(card_bg_color())
        .rounding(12.0f)
        .border(1.0f, border_light_color())
        .build();

    const float btn_w = 126.0f;
    const float btn_h = 32.0f;
    const float ctrl_center_y = ctrl_y + (ctrl_h - btn_h) * 0.5f;

    // Start/Stop toggle
    if (state.recognition_running) {
        ui.button("recog.ctrl.stop")
            .position(area.x + 14.0f, ctrl_center_y)
            .size(btn_w, btn_h)
            .style(ButtonStyle::Default)
            .text(tr(locale, "recognition.stop"))
            .fontSize(kFontButton)
            .icon("\xEF\x81\xB3")  // fa-pause
            .textColor(accent_danger_color())
            .onClick([]() {
                services::backend().stop_recognition();
                update_state([](const AppState& s) {
                    return with_recognition_running(
                        with_recognition_status(s, "stopped"), false);
                });
            })
            .build();
    } else {
        ui.button("recog.ctrl.start")
            .position(area.x + 14.0f, ctrl_center_y)
            .size(btn_w, btn_h)
            .style(ButtonStyle::Primary)
            .text(tr(locale, "recognition.start"))
            .fontSize(kFontButton)
            .icon("\xEF\x81\xA5")  // fa-play
            .onClick([]() {
                int cam = app_state().config.selected_camera;
                services::backend().start_recognition(cam);
                update_state([](const AppState& s) {
                    return with_recognition_running(
                        with_recognition_status(s, "running"), true);
                });
            })
            .build();
    }

    // Capture frame button
    ui.button("recog.ctrl.capture")
        .position(area.x + 14.0f + btn_w + 12.0f, ctrl_center_y)
        .size(btn_w, btn_h)
        .style(ButtonStyle::Outline)
        .text(tr(locale, "recognition.capture"))
        .fontSize(kFontButton)
        .icon("\xEF\x80\xB0")  // fa-camera
        .build();

    // Status indicator on the right
    const auto status_x = area.x + area.width - 170.0f;
    const auto status_color = state.recognition_running ? accent_success_color() : text_muted_color();
    const auto status_icon = state.recognition_running ? "\xEF\x84\x9F" : "\xEF\x81\xB3";
    const auto status_text = std::format("{} {}",
        status_icon,
        state.recognition_running
            ? tr(locale, "recognition.status_active")
            : tr(locale, "recognition.status_idle"));
    ui.label("recog.ctrl.status")
        .position(status_x, baseline_for_center(status_text, 14.0f, ctrl_y + ctrl_h * 0.5f))
        .fontSize(kFontMeta)
        .color(status_color)
        .text(status_text)
        .build();
}

}  // namespace (anonymous)

// ============================================================================
// SETTINGS PAGE — Configuration rows
// ============================================================================

namespace {

void render_settings_interactive(UIContext& ui, const RectFrame& area, const AppState& state) {
    const auto& locale = state.active_locale;
    const float row_h = 56.0f;
    const float label_w = 240.0f;
    const float control_w = std::max(200.0f, area.width - label_w - kCardGap * 3.0f);
    float current_y = area.y;

    // Helper: render a labeled settings row
    auto render_row = [&](const std::string& id, std::string_view label_text,
                          auto render_control) {
        ui.panel(std::format("settings.{}.bg", id))
            .position(area.x, current_y)
            .size(area.width, row_h)
            .background(card_bg_color())
            .rounding(10.0f)
            .border(1.0f, border_light_color())
            .build();

        ui.label(std::format("settings.{}.label", id))
            .position(area.x + 18.0f, baseline_for_center(label_text, kFontBodyStrong, current_y + row_h * 0.5f))
            .fontSize(kFontBodyStrong)
            .color(text_primary_color())
            .text(std::string(label_text))
            .build();

        render_control(area.x + label_w, current_y);

        current_y += row_h + kCardGap;
    };

    // Row 0: Camera selection
    render_row("camera", tr(locale, "settings.select_camera"),
        [&](float cx, float cy) {
            auto cameras = services::backend().enumerate_cameras();
            auto items = std::vector<std::string>{};
            std::ranges::transform(cameras, std::back_inserter(items),
                [](const CameraInfo& cam) {
                    return std::format("#{} - {}", cam.index, cam.name);
                });

            ui.combo("settings.camera.combo")
                .position(cx, cy + (row_h - 36.0f) * 0.5f)
                .size(control_w, 38.0f)
                .items(items)
                .selected(state.config.selected_camera)
                .fontSize(kFontBody)
                .onChange([](int index) {
                    update_state([index](const AppState& s) {
                        return with_selected_camera(s, index);
                    });
                })
                .build();
        });

    // Row 1: Recognition threshold
    render_row("threshold", tr(locale, "settings.recognition_threshold"),
        [&](float cx, float cy) {
            ui.slider("settings.threshold.slider")
                .position(cx, cy + (row_h - 20.0f) * 0.5f)
                .size(control_w * 0.6f, 20.0f)
                .value(state.config.recognition_threshold)
                .onChange([](float val) {
                    update_state([val](const AppState& s) {
                        return with_threshold(s, val);
                    });
                })
                .build();

            ui.label("settings.threshold.value")
                .position(cx + control_w * 0.6f + 16.0f,
                          baseline_for_center(std::format("{:.0f}%", state.config.recognition_threshold * 100.0f),
                                              kFontBody, cy + row_h * 0.5f))
                .fontSize(kFontBody)
                .color(text_secondary_color())
                .text(std::format("{:.0f}%", state.config.recognition_threshold * 100.0f))
                .build();
        });

    // Row 2: Liveness detection toggle
    render_row("liveness", tr(locale, "settings.liveness_detection"),
        [&](float cx, float cy) {
            ui.switcher("settings.liveness.switch")
                .position(cx, cy + (row_h - 24.0f) * 0.5f)
                .size(46.0f, 24.0f)
                .checked(state.config.liveness_detection)
                .label(state.config.liveness_detection
                    ? tr(locale, "settings.enabled")
                    : tr(locale, "settings.disabled"))
                .fontSize(kFontBody)
                .onChange([](bool val) {
                    update_state([val](const AppState& s) {
                        return with_liveness(s, val);
                    });
                })
                .build();
        });

    // Row 3: Language selection
    render_row("language", tr(locale, "settings.language"),
        [&](float cx, float cy) {
            const auto& store = i18n::app_i18n();
            auto lang_items = std::vector<std::string>{};
            std::ranges::transform(store.config.locales, std::back_inserter(lang_items),
                [](const auto& desc) { return desc.display_name; });

            int current_lang = 0;
            for (size_t i = 0; i < store.config.locales.size(); ++i) {
                if (store.config.locales[i].id == state.active_locale) {
                    current_lang = static_cast<int>(i);
                    break;
                }
            }

            ui.combo("settings.lang.combo")
                .position(cx, cy + (row_h - 36.0f) * 0.5f)
                .size(control_w, 38.0f)
                .items(lang_items)
                .selected(current_lang)
                .fontSize(kFontBody)
                .onChange([&store](int index) {
                    if (index >= 0 && index < static_cast<int>(store.config.locales.size())) {
                        const auto& new_locale = store.config.locales[index].id;
                        update_state([&new_locale](const AppState& s) {
                            return with_active_locale(s, new_locale);
                        });
                    }
                })
                .build();
        });

    // Row 4: Show diagnostics
    render_row("diagnostics", tr(locale, "settings.show_diagnostics"),
        [&](float cx, float cy) {
            ui.switcher("settings.diag.switch")
                .position(cx, cy + (row_h - 24.0f) * 0.5f)
                .size(46.0f, 24.0f)
                .checked(state.show_diagnostics)
                .label(state.show_diagnostics
                    ? tr(locale, "settings.diag_on")
                    : tr(locale, "settings.diag_off"))
                .fontSize(kFontBody)
                .onChange([](bool val) {
                    update_state([val](const AppState& s) {
                        return with_show_diagnostics(s, val);
                    });
                })
                .build();
        });

    // Diagnostics panel (collapsible)
    if (state.show_diagnostics) {
        ui.panel("settings.diag.panel")
            .position(area.x, current_y)
            .size(area.width, 90.0f)
            .background(card_bg_color())
            .rounding(10.0f)
            .border(1.0f, border_light_color())
            .build();

        const auto diag_text = std::format("i18n: {}\nCamera: Connected (#{})\nThreshold: {:.0f}%\nUsers: {} / Faces: {}",
            i18n::diagnostics_text(i18n::app_i18n()),
            state.config.selected_camera,
            state.config.recognition_threshold * 100.0f,
            state.total_users, state.total_faces);

        ui.label("settings.diag.content")
            .position(area.x + 16.0f, current_y + 12.0f)
            .fontSize(kFontMeta)
            .color(text_muted_color())
            .text(diag_text)
            .build();

        current_y += 90.0f + kCardGap;
    }

    // Footer: save and reset buttons
    current_y += 8.0f;
    ui.button("settings.save")
        .position(area.x, current_y)
        .size(136.0f, 36.0f)
        .style(ButtonStyle::Primary)
        .text(tr(locale, "settings.save"))
        .fontSize(kFontButton)
        .onClick([]() {
            const auto& s = app_state();
            services::backend().save_config(s.config);
        })
        .build();

    ui.button("settings.reset")
        .position(area.x + 160.0f, current_y)
        .size(136.0f, 36.0f)
        .style(ButtonStyle::Default)
        .text(tr(locale, "settings.reset"))
        .fontSize(kFontButton)
        .onClick([]() {
            update_state([](const AppState& s) {
                return with_threshold(
                    with_selected_camera(
                        with_liveness(s, true), 0), 0.65f);
            });
        })
        .build();
}

}  // namespace (anonymous)

}  // namespace su::app::ui
