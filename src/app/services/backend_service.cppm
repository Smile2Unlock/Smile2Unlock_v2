module;

#include <eui/EUINEO.h>

export module su.app.services.backend;

import std;
import su.app.state;

export namespace su::app::services {

/**
 * @brief 后端服务接口（GUI 端）
 * 
 * 为 GUI 提供后端操作的门面接口。
 * 当前使用模拟实现，后续可替换为真实的 IPC 调用。
 */
class IBackendService {
public:
    virtual ~IBackendService() = default;

    // 用户管理
    /**
     * @brief 获取所有用户
     * @return std::vector<UserEntry> 用户列表
     */
    virtual std::vector<UserEntry> get_users() = 0;
    
    /**
     * @brief 添加新用户
     * @param username 用户名
     * @param remark 备注
     * @return int 新用户ID
     */
    virtual int add_user(std::string_view username, std::string_view remark) = 0;
    
    /**
     * @brief 删除用户
     * @param user_id 用户ID
     * @return bool 是否成功
     */
    virtual bool delete_user(int user_id) = 0;

    // 人脸录入
    /**
     * @brief 捕获并添加人脸
     * @param user_id 用户ID
     * @return bool 是否成功
     */
    virtual bool capture_and_add_face(int user_id) = 0;
    
    /**
     * @brief 删除人脸
     * @param user_id 用户ID
     * @param face_id 人脸ID
     * @return bool 是否成功
     */
    virtual bool delete_face(int user_id, int face_id) = 0;

    // 摄像头管理
    /**
     * @brief 枚举可用摄像头
     * @return std::vector<CameraInfo> 摄像头列表
     */
    virtual std::vector<CameraInfo> enumerate_cameras() = 0;

    // 识别控制
    /**
     * @brief 开始人脸识别
     * @param camera_index 摄像头索引
     * @return bool 是否成功
     */
    virtual bool start_recognition(int camera_index) = 0;
    
    /**
     * @brief 停止人脸识别
     * @return bool 是否成功
     */
    virtual bool stop_recognition() = 0;
    
    /**
     * @brief 开始摄像头预览
     * @param camera_index 摄像头索引
     * @param fps 帧率
     * @return bool 是否成功
     */
    virtual bool start_preview(int camera_index, int fps = 15) = 0;
    
    /**
     * @brief 停止摄像头预览
     * @return bool 是否成功
     */
    virtual bool stop_preview() = 0;

    // 配置管理
    /**
     * @brief 加载配置
     * @return AppConfig 应用配置
     */
    virtual AppConfig load_config() = 0;
    
    /**
     * @brief 保存配置
     * @param config 应用配置
     */
    virtual void save_config(const AppConfig& config) = 0;
};

// --- 模拟实现（用于 GUI 开发）---

/**
 * @brief 模拟后端服务实现
 * 
 * 提供硬编码的测试数据，用于 GUI 开发阶段的界面调试。
 */
class MockBackendService final : public IBackendService {
public:
    /**
     * @brief 获取所有用户（模拟数据）
     */
    std::vector<UserEntry> get_users() override {
        return {
            {.user_id = 1, .username = "Alice", .remark = "Admin", .face_count = 2},
            {.user_id = 2, .username = "Bob", .remark = "Staff", .face_count = 2},
            {.user_id = 3, .username = "Carol", .remark = "Guest", .face_count = 1},
        };
    }

    /**
     * @brief 添加新用户（模拟实现）
     */
    int add_user(std::string_view username, std::string_view remark) override {
        static int next_id = 100;
        auto user = UserEntry{
            .user_id = next_id,
            .username = std::string(username),
            .remark = std::string(remark),
            .face_count = 0,
        };
        next_user_id_ = next_id++;
        return next_user_id_;
    }

    /**
     * @brief 删除用户（模拟实现）
     */
    bool delete_user(int user_id) override {
        return user_id > 0;
    }

    /**
     * @brief 捕获并添加人脸（模拟实现）
     */
    bool capture_and_add_face(int user_id) override {
        (void)user_id;
        return true;
    }

    /**
     * @brief 删除人脸（模拟实现）
     */
    bool delete_face(int user_id, int face_id) override {
        (void)user_id;
        (void)face_id;
        return true;
    }

    /**
     * @brief 枚举可用摄像头（模拟数据）
     */
    std::vector<CameraInfo> enumerate_cameras() override {
        return {
            {.index = 0, .name = "Built-in Camera (V4L2)"},
            {.index = 1, .name = "USB Camera 1080p"},
        };
    }

    /**
     * @brief 开始人脸识别（模拟实现）
     */
    bool start_recognition(int camera_index) override {
        (void)camera_index;
        return true;
    }

    /**
     * @brief 停止人脸识别（模拟实现）
     */
    bool stop_recognition() override {
        return true;
    }

    /**
     * @brief 开始摄像头预览（模拟实现）
     */
    bool start_preview(int camera_index, int fps) override {
        (void)camera_index;
        (void)fps;
        return true;
    }

    /**
     * @brief 停止摄像头预览（模拟实现）
     */
    bool stop_preview() override {
        return true;
    }

    /**
     * @brief 加载配置（模拟实现）
     */
    AppConfig load_config() override {
        return AppConfig{
            .selected_camera = 0,
            .recognition_threshold = 0.65f,
            .liveness_detection = true,
        };
    }

    /**
     * @brief 保存配置（模拟实现）
     */
    void save_config(const AppConfig& config) override {
        (void)config;
    }

private:
    int next_user_id_ = 100;
};

/**
 * @brief 获取后端服务单例
 * @return IBackendService& 后端服务实例引用
 */
inline IBackendService& backend() {
    static MockBackendService instance;
    return instance;
}

}  // namespace su::app::services
