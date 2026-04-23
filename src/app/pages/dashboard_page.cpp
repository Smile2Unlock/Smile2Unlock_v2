#include "page_content.h"

#include "../i18n/i18n.h"

namespace su::app {
namespace {

std::string tr(std::string_view key) {
    return i18n::text(i18n::app_i18n(), app_state().active_locale, key);
}

template <typename... Args>
std::string trf(std::string_view key, Args&&... args) {
    return i18n::format(i18n::app_i18n(), app_state().active_locale, key, std::forward<Args>(args)...);
}

PageContent make_dashboard_content() {
    return PageContent{
        .id = PageId::Dashboard,
        .title = tr("page.dashboard.title"),
        .description = tr("page.dashboard.description"),
        .primary_card = {.title = tr("content.card.primary.title"), .body = tr("content.card.primary.body")},
        .secondary_card = {.title = tr("content.card.secondary.title"), .body = tr("content.card.secondary.body")},
        .stage_title = tr("content.stage.title"),
        .stage_body = trf("content.stage.body", tr("page.dashboard.title")),
    };
}

}  // namespace

PageContent make_dashboard_page_content() {
    return make_dashboard_content();
}

}  // namespace su::app
