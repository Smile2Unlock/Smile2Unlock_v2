#include "inicpp.hpp" // 确保这个头文件在你的编译路径中
#include <string>

/**
 * @brief 用于管理 core 模块配置的类
 * 配置文件格式如下：
 * [core]
 * camera = 0
 * liveness = true
 * face_threshold = 0.62f
 * liveness_threshold = 0.8f
 * debug = false
 */
class ConfigManager {
public:
    /**
     * @brief 核心配置参数的结构体
     */
    struct CoreConfig {
        int camera;                 ///< 摄像头索引
        bool liveness;              ///< 是否启用活体检测
        float face_threshold;       ///< 人脸检测阈值
        float liveness_threshold;   ///< 活体检测阈值
        bool debug;                 ///< 调试模式开关
    };

    /**
     * @brief 构造函数
     * @param filename 配置文件的路径
     */
    explicit ConfigManager(const std::string& filename);

    /**
     * @brief 从配置文件加载配置
     * @return 成功返回 true, 否则返回 false (例如文件不存在或解析失败)
     */
    bool loadConfig();

    /**
     * @brief 将当前内存中的配置保存到文件
     * @return 成功返回 true, 否则返回 false
     */
    bool saveConfig() const;

    /**
     * @brief 获取当前配置
     * @return CoreConfig 结构体的常量引用
     */
    const CoreConfig& getConfig() const;

    /**
     * @brief 设置新的配置（仅在内存中）
     * @param newConfig 新的 CoreConfig 结构体
     */
    void setConfig(const CoreConfig& newConfig);

    /**
     * @brief 创建一个默认配置并保存到文件
     * @return 成功返回 true, 否则返回 false
     */
    bool createDefaultConfig() const;

private:
    std::string m_filename;  ///< 配置文件名
    CoreConfig m_config;     ///< 当前配置数据
    inicpp::IniManager m_ini; ///< inicpp 库的实例
};
