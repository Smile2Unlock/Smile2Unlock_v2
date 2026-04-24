export module su.app.state;

import std;
import su.app.i18n;

export namespace su::app {

enum class PageId {
    Dashboard,
    Enrollment,
    Recognition,
    Settings,
    Status,
};

struct AppState {
    PageId active_page = PageId::Dashboard;
    std::string active_locale;
    bool sidebar_collapsed = false;
};

struct NavItem {
    PageId page;
    std::string id;
    std::string icon;
    std::string label;
};

AppState make_initial_state();
AppState& app_state();
void update_state(const std::function<AppState(const AppState&)>& transform);
AppState with_active_page(const AppState& state, PageId page);
AppState with_active_locale(const AppState& state, std::string locale);
AppState with_sidebar_collapsed(const AppState& state, bool collapsed);
std::string page_name(PageId page);
std::vector<NavItem> make_sidebar_nav_items(const AppState& state);

}  // namespace su::app

namespace su::app {

inline AppState make_initial_state() {
    return AppState{
        .active_page = PageId::Dashboard,
        .active_locale = i18n::default_locale(i18n::app_i18n()),
        .sidebar_collapsed = false,
    };
}

inline AppState& app_state() {
    static AppState state = make_initial_state();
    return state;
}

inline void update_state(const std::function<AppState(const AppState&)>& transform) {
    app_state() = transform(app_state());
}

inline AppState with_active_page(const AppState& state, PageId page) {
    auto next = state;
    next.active_page = page;
    return next;
}

inline AppState with_active_locale(const AppState& state, std::string locale) {
    auto next = state;
    next.active_locale = std::move(locale);
    return next;
}

inline AppState with_sidebar_collapsed(const AppState& state, bool collapsed) {
    auto next = state;
    next.sidebar_collapsed = collapsed;
    return next;
}

inline std::string page_name(PageId page) {
    switch (page) {
        case PageId::Dashboard:
            return "dashboard";
        case PageId::Enrollment:
            return "enrollment";
        case PageId::Recognition:
            return "recognition";
        case PageId::Settings:
            return "settings";
        case PageId::Status:
            return "status";
    }
    return "dashboard";
}

inline std::string page_icon(PageId page) {
    switch (page) {
        case PageId::Dashboard:
            return "\xEF\x83\x9B";  // fa-columns
        case PageId::Enrollment:
            return "\xEF\x88\xB4";  // fa-user-plus
        case PageId::Recognition:
            return "\xEF\x80\xB0";  // fa-camera
        case PageId::Settings:
            return "\xEF\x80\x93";  // fa-cog
        case PageId::Status:
            return "\xEF\x88\x81";  // fa-line-chart
    }
    return "\xEF\x83\x9B";
}

inline std::vector<NavItem> make_sidebar_nav_items(const AppState& state) {
    const auto& store = i18n::app_i18n();
    static constexpr std::array nav_items{
        std::pair{PageId::Dashboard, "nav.dashboard"},
        std::pair{PageId::Enrollment, "nav.enrollment"},
        std::pair{PageId::Recognition, "nav.recognition"},
        std::pair{PageId::Settings, "nav.settings"},
        std::pair{PageId::Status, "nav.status"},
    };

    auto items = std::vector<NavItem>{};
    items.reserve(nav_items.size());
    std::ranges::transform(nav_items, std::back_inserter(items), [&](const auto& item) {
        return NavItem{
            .page = item.first,
            .id = std::format("nav.{}", page_name(item.first)),
            .icon = page_icon(item.first),
            .label = i18n::text(store, state.active_locale, item.second),
        };
    });
    return items;
}

}  // namespace su::app
