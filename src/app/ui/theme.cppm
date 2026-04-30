module;

#include <eui/app/DslAppRuntime.h>

export module su.app.ui.theme;

import std;

export namespace su::app::ui {

using EUINEO::Color;
using EUINEO::RectFrame;

// 窗口尺寸常量
constexpr int kWindowWidth = 1280;               ///< 窗口宽度
constexpr int kWindowHeight = 800;               ///< 窗口高度
constexpr float kTitleBarHeight = 40.0f;         ///< 标题栏高度
constexpr float kSidebarWidth = 208.0f;          ///< 侧边栏展开宽度
constexpr float kSidebarCollapsedWidth = 56.0f;  ///< 侧边栏折叠宽度
constexpr float kContentPadding = 20.0f;         ///< 内容区内边距
constexpr float kCardGap = 16.0f;                ///< 卡片间距
constexpr float kCardMinWidth = 280.0f;          ///< 卡片最小宽度
constexpr float kWindowButtonSize = 28.0f;       ///< 窗口控制按钮尺寸
constexpr float kWindowButtonGap = 8.0f;         ///< 窗口控制按钮间距
constexpr float kSidebarToggleSize = 36.0f;      ///< 侧边栏切换按钮尺寸
constexpr float kNavItemHeight = 44.0f;          ///< 导航项高度
constexpr float kNavItemGap = 4.0f;              ///< 导航项间距

// 颜色函数
Color title_bar_color();            ///< 标题栏背景色
Color title_bar_hover_color();      ///< 标题栏悬停色
Color window_button_close_hover();  ///< 关闭按钮悬停色
Color window_button_hover();        ///< 窗口按钮悬停色
Color sidebar_color();              ///< 侧边栏背景色
Color sidebar_hover_color();        ///< 侧边栏悬停色
Color sidebar_active_color();       ///< 侧边栏活动色
Color content_bg_color();           ///< 内容区背景色
Color card_bg_color();              ///< 卡片背景色
Color card_hover_color();           ///< 卡片悬停色
Color text_primary_color();         ///< 主文本色
Color text_secondary_color();       ///< 次文本色
Color text_muted_color();           ///< 弱化文本色
Color accent_primary_color();       ///< 主强调色
Color accent_success_color();       ///< 成功色
Color accent_warning_color();       ///< 警告色
Color accent_danger_color();        ///< 危险色
Color border_light_color();         ///< 浅色边框色

// 布局计算函数
RectFrame make_title_bar_frame(const RectFrame& window);   ///< 计算标题栏区域
RectFrame make_sidebar_frame(const RectFrame& window, bool sidebar_collapsed);  ///< 计算侧边栏区域
RectFrame make_content_frame(const RectFrame& window, bool sidebar_collapsed);  ///< 计算内容区区域

}  // namespace su::app::ui

namespace su::app::ui {

/**
 * @brief 标题栏背景色
 */
Color title_bar_color() {
    return Color(0.15f, 0.15f, 0.18f, 1.0f);
}

/**
 * @brief 标题栏悬停色
 */
Color title_bar_hover_color() {
    return Color(0.20f, 0.20f, 0.24f, 1.0f);
}

/**
 * @brief 关闭按钮悬停色
 */
Color window_button_close_hover() {
    return Color(0.85f, 0.23f, 0.23f, 1.0f);
}

/**
 * @brief 窗口按钮悬停色
 */
Color window_button_hover() {
    return Color(0.35f, 0.35f, 0.38f, 1.0f);
}

/**
 * @brief 侧边栏背景色
 */
Color sidebar_color() {
    return Color(0.18f, 0.18f, 0.22f, 1.0f);
}

/**
 * @brief 侧边栏悬停色
 */
Color sidebar_hover_color() {
    return Color(0.22f, 0.22f, 0.26f, 1.0f);
}

/**
 * @brief 侧边栏活动色
 */
Color sidebar_active_color() {
    return Color(0.25f, 0.45f, 0.75f, 1.0f);
}

/**
 * @brief 内容区背景色
 */
Color content_bg_color() {
    return Color(0.12f, 0.12f, 0.14f, 1.0f);
}

/**
 * @brief 卡片背景色
 */
Color card_bg_color() {
    return Color(0.20f, 0.20f, 0.24f, 1.0f);
}

/**
 * @brief 卡片悬停色
 */
Color card_hover_color() {
    return Color(0.24f, 0.24f, 0.28f, 1.0f);
}

/**
 * @brief 主文本色
 */
Color text_primary_color() {
    return Color(0.95f, 0.95f, 0.97f, 1.0f);
}

/**
 * @brief 次文本色
 */
Color text_secondary_color() {
    return Color(0.70f, 0.72f, 0.75f, 1.0f);
}

/**
 * @brief 弱化文本色
 */
Color text_muted_color() {
    return Color(0.50f, 0.52f, 0.55f, 1.0f);
}

/**
 * @brief 主强调色
 */
Color accent_primary_color() {
    return Color(0.30f, 0.55f, 0.95f, 1.0f);
}

/**
 * @brief 成功色
 */
Color accent_success_color() {
    return Color(0.20f, 0.75f, 0.45f, 1.0f);
}

/**
 * @brief 警告色
 */
Color accent_warning_color() {
    return Color(0.90f, 0.65f, 0.20f, 1.0f);
}

/**
 * @brief 危险色
 */
Color accent_danger_color() {
    return Color(0.85f, 0.25f, 0.30f, 1.0f);
}

/**
 * @brief 浅色边框色
 */
Color border_light_color() {
    return Color(0.35f, 0.36f, 0.40f, 0.60f);
}

/**
 * @brief 计算标题栏区域
 */
RectFrame make_title_bar_frame(const RectFrame& window) {
    return RectFrame{0.0f, 0.0f, window.width, kTitleBarHeight};
}

/**
 * @brief 计算侧边栏区域
 */
RectFrame make_sidebar_frame(const RectFrame& window, bool sidebar_collapsed) {
    const auto sidebar_width = sidebar_collapsed ? kSidebarCollapsedWidth : kSidebarWidth;
    return RectFrame{0.0f, kTitleBarHeight, sidebar_width, window.height - kTitleBarHeight};
}

/**
 * @brief 计算内容区区域
 */
RectFrame make_content_frame(const RectFrame& window, bool sidebar_collapsed) {
    const auto sidebar_width = sidebar_collapsed ? kSidebarCollapsedWidth : kSidebarWidth;
    return RectFrame{sidebar_width, kTitleBarHeight, window.width - sidebar_width, window.height - kTitleBarHeight};
}

}  // namespace su::app::ui
