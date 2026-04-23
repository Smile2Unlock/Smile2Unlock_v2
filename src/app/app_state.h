#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace su::app {

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
