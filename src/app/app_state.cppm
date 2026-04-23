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
};

AppState make_initial_state();
AppState& app_state();
void update_state(const std::function<AppState(const AppState&)>& transform);
AppState with_active_page(const AppState& state, PageId page);
AppState with_active_locale(const AppState& state, std::string locale);
std::string page_name(PageId page);

}  // namespace su::app

namespace su::app {

AppState make_initial_state() {
    return AppState{
        .active_page = PageId::Dashboard,
        .active_locale = i18n::default_locale(i18n::app_i18n()),
    };
}

AppState& app_state() {
    static AppState state = make_initial_state();
    return state;
}

void update_state(const std::function<AppState(const AppState&)>& transform) {
    app_state() = transform(app_state());
}

AppState with_active_page(const AppState& state, PageId page) {
    auto next = state;
    next.active_page = page;
    return next;
}

AppState with_active_locale(const AppState& state, std::string locale) {
    auto next = state;
    next.active_locale = std::move(locale);
    return next;
}

std::string page_name(PageId page) {
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

}  // namespace su::app
