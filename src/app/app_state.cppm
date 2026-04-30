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

/**
 * @brief 创建初始应用程序状态
 * @return AppState 初始化后的应用状态
 */
AppState make_initial_state();

/**
 * @brief 获取全局应用程序状态单例
 * @return AppState& 全局应用状态的引用
 */
const AppState& app_state();

/**
 * @brief 使用转换函数更新应用程序状态
 * @param transform 状态转换函数，接收当前状态并返回新状态
 */
void update_state(const std::function<AppState(const AppState&)>& transform);

// Shell
/**
 * @brief 设置当前活动页面
 * @param state 当前状态
 * @param page 目标页面ID
 * @return AppState 更新后的状态
 */
AppState with_active_page(const AppState& state, PageId page);

/**
 * @brief 设置活动语言环境
 * @param state 当前状态
 * @param locale 语言环境标识（如 "zh-CN", "en-US"）
 * @return AppState 更新后的状态
 */
AppState with_active_locale(const AppState& state, std::string locale);

/**
 * @brief 设置侧边栏折叠状态
 * @param state 当前状态
 * @param collapsed 是否折叠
 * @return AppState 更新后的状态
 */
AppState with_sidebar_collapsed(const AppState& state, bool collapsed);

// User management
/**
 * @brief 更新用户列表
 * @param state 当前状态
 * @param users 新的用户列表
 * @return AppState 更新后的状态（会自动更新 total_users）
 */
AppState with_users(const AppState& state, std::vector<UserEntry> users);

/**
 * @brief 设置选中的用户ID
 * @param state 当前状态
 * @param user_id 用户ID，-1表示未选中
 * @return AppState 更新后的状态
 */
AppState with_selected_user(const AppState& state, int user_id);

/**
 * @brief 设置是否显示添加用户对话框
 * @param state 当前状态
 * @param show 是否显示
 * @return AppState 更新后的状态
 */
AppState with_show_add_user(const AppState& state, bool show);

/**
 * @brief 设置是否正在采集人脸
 * @param state 当前状态
 * @param capturing 是否正在采集
 * @return AppState 更新后的状态
 */
AppState with_capturing_face(const AppState& state, bool capturing);

// Recognition
/**
 * @brief 设置识别运行状态
 * @param state 当前状态
 * @param running 是否正在运行
 * @return AppState 更新后的状态
 */
AppState with_recognition_running(const AppState& state, bool running);

/**
 * @brief 设置识别状态文本
 * @param state 当前状态
 * @param status 状态文本（如 "idle", "running", "error"）
 * @return AppState 更新后的状态
 */
AppState with_recognition_status(const AppState& state, std::string status);

/**
 * @brief 设置识别结果
 * @param state 当前状态
 * @param result 识别结果（包含成功状态、用户名、相似度、消息）
 * @return AppState 更新后的状态
 */
AppState with_recognition_result(const AppState& state, RecognitionResult result);

// Config
/**
 * @brief 更新完整的应用配置
 * @param state 当前状态
 * @param config 新的应用配置
 * @return AppState 更新后的状态（会同步 active_locale）
 */
AppState with_config(const AppState& state, AppConfig config);

/**
 * @brief 设置选中的摄像头索引
 * @param state 当前状态
 * @param camera_index 摄像头索引
 * @return AppState 更新后的状态
 */
AppState with_selected_camera(const AppState& state, int camera_index);

/**
 * @brief 设置识别阈值
 * @param state 当前状态
 * @param threshold 识别阈值（0.0-1.0）
 * @return AppState 更新后的状态
 */
AppState with_threshold(const AppState& state, float threshold);

/**
 * @brief 设置活体检测开关
 * @param state 当前状态
 * @param enabled 是否启用活体检测
 * @return AppState 更新后的状态
 */
AppState with_liveness(const AppState& state, bool enabled);

/**
 * @brief 设置是否显示诊断信息
 * @param state 当前状态
 * @param show 是否显示
 * @return AppState 更新后的状态
 */
AppState with_show_diagnostics(const AppState& state, bool show);

// Helpers
/**
 * @brief 导航项结构，用于侧边栏导航
 */
struct NavItem {
    PageId page;          ///< 页ID
    std::string id;       ///< 导航项ID
    std::string icon;     ///< 图标（Unicode字符）
    std::string label;    ///< 显示标签
};

/**
 * @brief 获取页面的英文名称
 * @param page 页面ID
 * @return std::string 页面英文名称
 */
std::string page_name(PageId page);

/**
 * @brief 创建侧边栏导航项列表
 * @param state 当前应用状态（用于获取当前语言）
 * @return std::vector<NavItem> 导航项列表
 */
std::vector<NavItem> make_sidebar_nav_items(const AppState& state);

}  // namespace su::app

namespace su::app {

/**
 * @brief 创建初始应用程序状态
 * @return AppState 初始化后的应用状态
 */
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


/**
 * @brief 内部可变状态访问器
 */
inline AppState& mutable_state() {
    static AppState state = make_initial_state();
    return state;
}

/**
 * @brief 获取全局应用程序状态（只读）
 * @return const AppState& 全局应用状态的常量引用
 */
inline const AppState& app_state() {
    return mutable_state();
}

inline void update_state(const std::function<AppState(const AppState&)>& transform) {
    mutable_state() = transform(mutable_state());
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
}

inline std::vector<NavItem> make_sidebar_nav_items(const AppState& state) {
    const auto& store = i18n::app_i18n();
    static constexpr std::array nav_items{
        std::pair{PageId::Dashboard, "nav.dashboard"},
        std::pair{PageId::Enrollment, "nav.enrollment"},
        std::pair{PageId::Recognition, "nav.recognition"},
        std::pair{PageId::Settings, "nav.settings"},
    };

    return nav_items
        | std::views::transform([&](const auto& item) {
            return NavItem{
                .page = item.first,
                .id = std::format("nav.{}", page_name(item.first)),
                .icon = page_icon(item.first),
                .label = i18n::text(store, state.active_locale, item.second),
            };
        })
        | std::ranges::to<std::vector>();
}

}  // namespace su::app
