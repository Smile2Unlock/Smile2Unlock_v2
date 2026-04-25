export module su.app.state;

import std;
import su.app.i18n;

export namespace su::app {

enum class PageId {
    Dashboard,
    Enrollment,
    Recognition,
    Settings,
};

struct UserEntry {
    int user_id = 0;
    std::string username;
    std::string remark;
    int face_count = 0;
};

struct CameraInfo {
    int index = 0;
    std::string name;
};

struct RecognitionResult {
    bool success = false;
    std::string username;
    float similarity = 0.0f;
    std::string message;
};

struct AppConfig {
    int selected_camera = 0;
    float recognition_threshold = 0.65f;
    bool liveness_detection = true;
    std::string active_locale;
};

struct AppState {
    // Shell state
    PageId active_page = PageId::Dashboard;
    std::string active_locale;
    bool sidebar_collapsed = true;

    // Dashboard state
    int total_users = 3;
    int total_faces = 5;
    bool camera_connected = true;
    std::string system_status;

    // Enrollment state
    std::vector<UserEntry> users;
    int selected_user_id = -1;
    bool show_add_user = false;
    bool is_capturing_face = false;

    // Recognition state
    bool recognition_running = false;
    std::string recognition_status;
    RecognitionResult last_result;
    float preview_fps = 0.0f;

    // Settings state
    AppConfig config;
    bool show_diagnostics = false;
    std::string settings_diagnostics;
};

// --- Immutable state transformers ---

AppState make_initial_state();
AppState& app_state();
void update_state(const std::function<AppState(const AppState&)>& transform);

// Shell
AppState with_active_page(const AppState& state, PageId page);
AppState with_active_locale(const AppState& state, std::string locale);
AppState with_sidebar_collapsed(const AppState& state, bool collapsed);

// User management
AppState with_users(const AppState& state, std::vector<UserEntry> users);
AppState with_selected_user(const AppState& state, int user_id);
AppState with_show_add_user(const AppState& state, bool show);
AppState with_capturing_face(const AppState& state, bool capturing);

// Recognition
AppState with_recognition_running(const AppState& state, bool running);
AppState with_recognition_status(const AppState& state, std::string status);
AppState with_recognition_result(const AppState& state, RecognitionResult result);

// Config
AppState with_config(const AppState& state, AppConfig config);
AppState with_selected_camera(const AppState& state, int camera_index);
AppState with_threshold(const AppState& state, float threshold);
AppState with_liveness(const AppState& state, bool enabled);
AppState with_show_diagnostics(const AppState& state, bool show);

// Helpers
struct NavItem {
    PageId page;
    std::string id;
    std::string icon;
    std::string label;
};

std::string page_name(PageId page);
std::vector<NavItem> make_sidebar_nav_items(const AppState& state);

}  // namespace su::app

namespace su::app {

inline AppState make_initial_state() {
    return AppState{
        .active_page = PageId::Dashboard,
        .active_locale = i18n::default_locale(i18n::app_i18n()),
        .sidebar_collapsed = true,

        .total_users = 3,
        .total_faces = 5,
        .camera_connected = true,
        .system_status = "ready",

        .users = {
            {.user_id = 1, .username = "Alice", .remark = "Admin", .face_count = 2},
            {.user_id = 2, .username = "Bob", .remark = "Staff", .face_count = 2},
            {.user_id = 3, .username = "Carol", .remark = "Guest", .face_count = 1},
        },
        .selected_user_id = -1,
        .show_add_user = false,
        .is_capturing_face = false,

        .recognition_running = false,
        .recognition_status = "idle",

        .config = {
            .selected_camera = 0,
            .recognition_threshold = 0.65f,
            .liveness_detection = true,
            .active_locale = i18n::default_locale(i18n::app_i18n()),
        },
        .show_diagnostics = false,
    };
}

inline AppState& app_state() {
    static AppState state = make_initial_state();
    return state;
}

inline void update_state(const std::function<AppState(const AppState&)>& transform) {
    app_state() = transform(app_state());
}

// Shell
inline AppState with_active_page(const AppState& state, PageId page) {
    auto next = state;
    next.active_page = page;
    return next;
}

inline AppState with_active_locale(const AppState& state, std::string locale) {
    auto next = state;
    next.active_locale = std::move(locale);
    next.config.active_locale = next.active_locale;
    return next;
}

inline AppState with_sidebar_collapsed(const AppState& state, bool collapsed) {
    auto next = state;
    next.sidebar_collapsed = collapsed;
    return next;
}

// User management
inline AppState with_users(const AppState& state, std::vector<UserEntry> users) {
    auto next = state;
    next.users = std::move(users);
    next.total_users = static_cast<int>(next.users.size());
    return next;
}

inline AppState with_selected_user(const AppState& state, int user_id) {
    auto next = state;
    next.selected_user_id = user_id;
    return next;
}

inline AppState with_show_add_user(const AppState& state, bool show) {
    auto next = state;
    next.show_add_user = show;
    return next;
}

inline AppState with_capturing_face(const AppState& state, bool capturing) {
    auto next = state;
    next.is_capturing_face = capturing;
    return next;
}

// Recognition
inline AppState with_recognition_running(const AppState& state, bool running) {
    auto next = state;
    next.recognition_running = running;
    return next;
}

inline AppState with_recognition_status(const AppState& state, std::string status) {
    auto next = state;
    next.recognition_status = std::move(status);
    return next;
}

inline AppState with_recognition_result(const AppState& state, RecognitionResult result) {
    auto next = state;
    next.last_result = std::move(result);
    return next;
}

// Config
inline AppState with_config(const AppState& state, AppConfig config) {
    auto next = state;
    next.config = std::move(config);
    next.active_locale = next.config.active_locale;
    return next;
}

inline AppState with_selected_camera(const AppState& state, int camera_index) {
    auto next = state;
    next.config.selected_camera = camera_index;
    return next;
}

inline AppState with_threshold(const AppState& state, float threshold) {
    auto next = state;
    next.config.recognition_threshold = threshold;
    return next;
}

inline AppState with_liveness(const AppState& state, bool enabled) {
    auto next = state;
    next.config.liveness_detection = enabled;
    return next;
}

inline AppState with_show_diagnostics(const AppState& state, bool show) {
    auto next = state;
    next.show_diagnostics = show;
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
