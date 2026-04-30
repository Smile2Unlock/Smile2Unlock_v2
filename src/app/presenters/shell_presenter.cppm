export module su.app.presenters;

import std;
import su.app.i18n;
import su.app.pages;
import su.app.state;

export namespace su::app::presenters {

/**
 * @brief 头部视图模型
 */
struct HeaderViewModel {
    std::string title;                ///< 应用标题
    std::string subtitle;             ///< 副标题（包含语言信息）
    std::string locale_button_text;   ///< 语言切换按钮文本
    bool show_locale_switch = false;  ///< 是否显示语言切换按钮
};

/**
 * @brief 导航项视图模型
 */
struct NavItemViewModel {
    PageId page;             ///< 页ID
    std::string id;          ///< 导航项ID
    std::string label;       ///< 显示标签
    bool active = false;     ///< 是否为当前活动项
};

/**
 * @brief 侧边栏视图模型
 */
struct SidebarViewModel {
    std::string title;                          ///< 侧边栏标题
    std::string subtitle;                       ///< 侧边栏副标题
    std::vector<NavItemViewModel> items;        ///< 导航项列表
};

/**
 * @brief 内容区视图模型
 */
struct ContentViewModel {
    std::string title;              ///< 页面标题
    std::string description;        ///< 页面描述
    PageCardContent primary_card;   ///< 主卡片内容
    PageCardContent secondary_card; ///< 副卡片内容
    std::string stage_title;        ///< 舞台区标题
    std::string stage_body;         ///< 舞台区内容
    std::string diagnostics;        ///< 诊断信息
};

/**
 * @brief 完整外壳视图模型
 */
struct ShellViewModel {
    HeaderViewModel header;     ///< 头部视图模型
    SidebarViewModel sidebar;   ///< 侧边栏视图模型
    ContentViewModel content;   ///< 内容区视图模型
};

/**
 * @brief 构建完整的外壳视图模型
 * 
 * 根据当前应用状态和屏幕宽度，构建包含头部、侧边栏和内容区的完整视图模型。
 * 
 * @param state 当前应用状态
 * @param screen_width 屏幕宽度
 * @return ShellViewModel 完整的外壳视图模型
 */
ShellViewModel make_shell_view_model(const AppState& state, float screen_width);

}  // namespace su::app::presenters

namespace su::app::presenters {
namespace {

/**
 * @brief 翻译文本快捷函数
 */
std::string tr(const i18n::I18nStore& store, std::string_view locale, std::string_view key) {
    return i18n::text(store, locale, key);
}

/**
 * @brief 翻译并格式化文本快捷函数
 */
template <typename... Args>
std::string trf(const i18n::I18nStore& store, std::string_view locale, std::string_view key, Args&&... args) {
    return i18n::format(store, locale, key, std::forward<Args>(args)...);
}

/**
 * @brief 构建导航项列表
 * 
 * @param state 当前应用状态
 * @param store 国际化存储
 * @return std::vector<NavItemViewModel> 导航项视图模型列表
 */
std::vector<NavItemViewModel> make_nav_items(const AppState& state, const i18n::I18nStore& store) {
    static constexpr std::array nav_items{
        std::pair{PageId::Dashboard, "nav.dashboard"},
        std::pair{PageId::Enrollment, "nav.enrollment"},
        std::pair{PageId::Recognition, "nav.recognition"},
        std::pair{PageId::Settings, "nav.settings"},
    };

    auto items = std::vector<NavItemViewModel>{};
    items.reserve(nav_items.size());
    std::ranges::transform(nav_items, std::back_inserter(items), [&](const auto& item) {
        return NavItemViewModel{
            .page = item.first,
            .id = std::format("sidebar.{}", page_name(item.first)),
            .label = tr(store, state.active_locale, item.second),
            .active = state.active_page == item.first,
        };
    });
    return items;
}

}  // namespace

/**
 * @brief 构建完整的外壳视图模型
 * 
 * 根据当前应用状态和屏幕宽度，构建包含头部、侧边栏和内容区的完整视图模型。
 * 
 * @param state 当前应用状态
 * @param screen_width 屏幕宽度
 * @return ShellViewModel 完整的外壳视图模型
 */
ShellViewModel make_shell_view_model(const AppState& state, float screen_width) {
    const auto& store = i18n::app_i18n();
    const auto locale_name = i18n::locale_display_name(store, state.active_locale);
    const auto page = make_page_content(store, state.active_locale, state.active_page);

    return ShellViewModel{
        .header = {
            .title = tr(store, state.active_locale, "app.title"),
            .subtitle = trf(store, state.active_locale, "header.subtitle", locale_name),
            .locale_button_text = trf(store, state.active_locale, "header.locale_button", locale_name),
            .show_locale_switch = screen_width >= 880.0f,
        },
        .sidebar = {
            .title = tr(store, state.active_locale, "sidebar.title"),
            .subtitle = tr(store, state.active_locale, "sidebar.subtitle"),
            .items = make_nav_items(state, store),
        },
        .content = {
            .title = page.title,
            .description = page.description,
            .primary_card = page.primary_card,
            .secondary_card = page.secondary_card,
            .stage_title = page.stage_title,
            .stage_body = page.stage_body,
            .diagnostics = trf(store, state.active_locale, "content.diagnostics.body", i18n::diagnostics_text(store)),
        },
    };
}

}  // namespace su::app::presenters
