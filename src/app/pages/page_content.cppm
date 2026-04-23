export module su.app.pages;

import std;
import su.app.i18n;
import su.app.state;

export namespace su::app {

struct PageCardContent {
    std::string title;
    std::string body;
};

struct PageContent {
    PageId id;
    std::string title;
    std::string description;
    PageCardContent primary_card;
    PageCardContent secondary_card;
    std::string stage_title;
    std::string stage_body;
};

PageContent make_page_content(const i18n::I18nStore& store, std::string_view locale, PageId page);

}  // namespace su::app

namespace su::app {
namespace {

std::string tr(const i18n::I18nStore& store, std::string_view locale, std::string_view key) {
    return i18n::text(store, locale, key);
}

template <typename... Args>
std::string trf(const i18n::I18nStore& store, std::string_view locale, std::string_view key, Args&&... args) {
    return i18n::format(store, locale, key, std::forward<Args>(args)...);
}

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
        case PageId::Status:
            return make_page(PageId::Status, store, locale, "page.status.title", "page.status.description");
    }
    return make_page(PageId::Dashboard, store, locale, "page.dashboard.title", "page.dashboard.description");
}

}  // namespace su::app
