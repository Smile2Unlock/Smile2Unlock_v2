export module su.app.pages;

import std;
import su.app.i18n;
import su.app.state;

export namespace su::app {

/**
 * @brief 页卡片内容
 */
struct PageCardContent {
    std::string title;  ///< 卡片标题
    std::string body;   ///< 卡片内容
};

/**
 * @brief 页内容
 */
struct PageContent {
    PageId id;                      ///< 页ID
    std::string title;              ///< 页标题
    std::string description;        ///< 页描述
    PageCardContent primary_card;   ///< 主卡片
    PageCardContent secondary_card; ///< 副卡片
    std::string stage_title;        ///< 舞台区标题
    std::string stage_body;         ///< 舞台区内容
};

/**
 * @brief 构建页内容
 * 
 * 根据页ID和当前语言，从国际化资源中加载对应的页内容。
 * 
 * @param store 国际化存储
 * @param locale 当前语言
 * @param page 页ID
 * @return PageContent 页内容
 */
PageContent make_page_content(const i18n::I18nStore& store, std::string_view locale, PageId page);

}  // namespace su::app

namespace su::app {
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
 * @brief 构建单个页内容
 * 
 * @param id 页ID
 * @param store 国际化存储
 * @param locale 当前语言
 * @param title_key 标题的国际化键
 * @param description_key 描述的国际化键
 * @return PageContent 页内容
 */
PageContent make_page(PageId id, const i18n::I18nStore& store, std::string_view locale, std::string_view title_key, std::string_view description_key) {
    const auto title = tr(store, locale, title_key);
    return PageContent{
        .id = id,
        .title = title,
        .description = tr(store, locale, description_key),
        .primary_card = {
            .title = tr(store, locale, "content.card.primary.title"),
            .body = tr(store, locale, "content.card.primary.body"),
        },
        .secondary_card = {
            .title = tr(store, locale, "content.card.secondary.title"),
            .body = tr(store, locale, "content.card.secondary.body"),
        },
        .stage_title = tr(store, locale, "content.stage.title"),
        .stage_body = trf(store, locale, "content.stage.body", title),
    };
}

}  // namespace

/**
 * @brief 构建页内容
 * 
 * 根据页ID分发到对应的页内容构建逻辑。
 * 
 * @param store 国际化存储
 * @param locale 当前语言
 * @param page 页ID
 * @return PageContent 页内容
 */
PageContent make_page_content(const i18n::I18nStore& store, std::string_view locale, PageId page) {
    switch (page) {
        case PageId::Dashboard:
            return make_page(PageId::Dashboard, store, locale, "page.dashboard.title", "page.dashboard.description");
        case PageId::Enrollment:
            return make_page(PageId::Enrollment, store, locale, "page.enrollment.title", "page.enrollment.description");
        case PageId::Recognition:
            return make_page(PageId::Recognition, store, locale, "page.recognition.title", "page.recognition.description");
        case PageId::Settings:
            return make_page(PageId::Settings, store, locale, "page.settings.title", "page.settings.description");
    }
    return make_page(PageId::Dashboard, store, locale, "page.dashboard.title", "page.dashboard.description");
}

}  // namespace su::app
