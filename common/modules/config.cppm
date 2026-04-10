//
// Created by ation_ciger on 2026/3/19.
//
module;

#include "inicpp.hpp"

export module config;

import std;

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
export class ConfigManager {
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
        std::string language;       ///< UI language code, empty means auto-detect

        CoreConfig()
            : camera(0)
            , liveness(true)
            , face_threshold(0.62f)
            , liveness_threshold(0.8f)
            , debug(false)
            , language() {}
    };

    /**
     * @brief UDP 端口配置
     */
    struct UdpPortConfig {
        uint16_t cp_status_port;       ///< CP 接收状态端口 (FR/SU 发送)
        uint16_t fr_status_port;       ///< SU 接收 FR 状态端口
        uint16_t auth_request_port;    ///< SU 接收认证请求端口
        uint16_t password_port;        ///< CP 接收密码响应端口

        UdpPortConfig()
            : cp_status_port(51234)
            , fr_status_port(51235)
            , auth_request_port(51236)
            , password_port(51237) {}
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

    /**
     * @brief 获取 UDP 端口配置（静态默认值）
     * @return UdpPortConfig 结构体的常量引用
     */
    static const UdpPortConfig& getUdpPorts() {
        static UdpPortConfig ports;
        return ports;
    }

private:
    std::string m_filename;  ///< 配置文件名
    CoreConfig m_config;     ///< 当前配置数据
    inicpp::IniManager m_ini; ///< inicpp 库的实例
};

// 实现部分
ConfigManager::ConfigManager(const std::string& filename)
    : m_filename(filename), m_ini(m_filename) {
    // 初始化默认值
    m_config.camera = 0;
    m_config.liveness = true;
    m_config.face_threshold = 0.62f;
    m_config.liveness_threshold = 0.8f;
    m_config.debug = false;
    m_config.language.clear();
}

bool ConfigManager::loadConfig() {
    // 尝试加载文件
    std::ifstream file(m_filename);
    if (!file.is_open()) {
        std::cerr << "Failed to load config file: " << m_filename << std::endl;
        return false;
    }

    // Check if the [core] section exists
    if (!m_ini.isSectionExists("core")) {
        std::cerr << "Warning: Section [core] not found in " << m_filename << ". Using defaults." << std::endl;
        return false;
    }

    // Get the [core] section object
    auto core_section = m_ini["core"];

    // Check if individual keys exist before accessing them
    if (!core_section.isKeyExist("camera")) {
        std::cerr << "Warning: Key 'camera' not found in [core]. Using default value (" << m_config.camera << ")." << std::endl;
    } else {
        m_config.camera = core_section.toInt("camera");
    }

    if (!core_section.isKeyExist("liveness")) {
        std::cerr << "Warning: Key 'liveness' not found in [core]. Using default value (" << m_config.liveness << ")." << std::endl;
    } else {
        std::string liveness_str = core_section.toString("liveness");
        if (liveness_str == "true" || liveness_str == "True" || liveness_str == "TRUE" || liveness_str == "1") {
            m_config.liveness = true;
        } else if (liveness_str == "false" || liveness_str == "False" || liveness_str == "FALSE" || liveness_str == "0") {
            m_config.liveness = false;
        } else {
             std::cerr << "Warning: Invalid value for 'liveness' ('" << liveness_str << "'). Using default value (" << m_config.liveness << ")." << std::endl;
        }
    }

    if (!core_section.isKeyExist("face_threshold")) {
        std::cerr << "Warning: Key 'face_threshold' not found in [core]. Using default value (" << m_config.face_threshold << ")." << std::endl;
    } else {
        m_config.face_threshold = static_cast<float>(core_section.toDouble("face_threshold"));
    }

    if (!core_section.isKeyExist("liveness_threshold")) {
        std::cerr << "Warning: Key 'liveness_threshold' not found in [core]. Using default value (" << m_config.liveness_threshold << ")." << std::endl;
    } else {
        m_config.liveness_threshold = static_cast<float>(core_section.toDouble("liveness_threshold"));
    }

    if (!core_section.isKeyExist("debug")) {
        std::cerr << "Warning: Key 'debug' not found in [core]. Using default value (" << m_config.debug << ")." << std::endl;
    } else {
        std::string debug_str = core_section.toString("debug");
        if (debug_str == "true" || debug_str == "True" || debug_str == "TRUE" || debug_str == "1") {
            m_config.debug = true;
        } else if (debug_str == "false" || debug_str == "False" || debug_str == "FALSE" || debug_str == "0") {
            m_config.debug = false;
        } else {
             std::cerr << "Warning: Invalid value for 'debug' ('" << debug_str << "'). Using default value (" << m_config.debug << ")." << std::endl;
        }
    }

    if (core_section.isKeyExist("language")) {
        m_config.language = core_section.toString("language");
    }

    return true;
}

const ConfigManager::CoreConfig& ConfigManager::getConfig() const {
    return m_config;
}

void ConfigManager::setConfig(const ConfigManager::CoreConfig& newConfig) {
    m_config = newConfig;
}

bool ConfigManager::createDefaultConfig() const {
    // 创建默认配置文件
    std::error_code ec;
    const auto path = std::filesystem::path(m_filename);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream file(m_filename);
    if (!file.is_open()) {
        std::cerr << "Failed to create config file: " << m_filename << std::endl;
        return false;
    }

    file << "[core]\n";
    file << "camera=" << m_config.camera << "\n";
    file << "liveness=" << (m_config.liveness ? "true" : "false") << "\n";
    file << "face_threshold=" << m_config.face_threshold << "\n";
    file << "liveness_threshold=" << m_config.liveness_threshold << "\n";
    file << "debug=" << (m_config.debug ? "true" : "false") << "\n";
    if (!m_config.language.empty()) {
        file << "language=" << m_config.language << "\n";
    }

    file.close();
    std::cout << "Default config created: " << m_filename << std::endl;
    return true;
}

bool ConfigManager::saveConfig() const {
    // 直接写入配置文件
    std::error_code ec;
    const auto path = std::filesystem::path(m_filename);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream file(m_filename);
    if (!file.is_open()) {
        std::cerr << "Failed to save config file: " << m_filename << std::endl;
        return false;
    }

    file << "[core]\n";
    file << "camera=" << m_config.camera << "\n";
    file << "liveness=" << (m_config.liveness ? "true" : "false") << "\n";
    file << "face_threshold=" << m_config.face_threshold << "\n";
    file << "liveness_threshold=" << m_config.liveness_threshold << "\n";
    file << "debug=" << (m_config.debug ? "true" : "false") << "\n";
    if (!m_config.language.empty()) {
        file << "language=" << m_config.language << "\n";
    }

    file.close();
    return true;
}
