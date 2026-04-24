export module su.app.presenters;

import std;
import su.app.i18n;
import su.app.pages;
import su.app.state;

export namespace su::app::presenters {

struct HeaderViewModel {
    std::string title;
    std::string subtitle;
    std::string locale_button_text;
    bool show_locale_switch = false;
};

struct NavItemViewModel {
    PageId page;
    std::string id;
    std::string label;
    bool active = false;
};

struct SidebarViewModel {
    std::string title;
    std::string subtitle;
    std::vector<NavItemViewModel> items;
};

struct ContentViewModel {
    std::string title;
    std::string description;
    PageCardContent primary_card;
    PageCardContent secondary_card;
    std::string stage_title;
    std::string stage_body;
    std::string diagnostics;
};

struct ShellViewModel {
    HeaderViewModel header;
    SidebarViewModel sidebar;
    ContentViewModel content;
};

ShellViewModel make_shell_view_model(const AppState& state, float screen_width);

}  // namespace su::app::presenters

namespace su::app::presenters {
namespace {

std::string tr(const i18n::I18nStore& store, std::string_view locale, std::string_view key) {
    return i18n::text(store, locale, key);
}

template <typename... Args>
std::string trf(const i18n::I18nStore& store, std::string_view locale, std::string_view key, Args&&... args) {
    return i18n::format(store, locale, key, std::forward<Args>(args)...);
}

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
