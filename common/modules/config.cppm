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
 * target_fps = 30
 * enable_fps_limit = true
 * skip_frames = 1
 * enable_async_processing = false
 * cache_features = true
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

        // 性能优化配置
        int target_fps;             ///< 目标FPS（帧率限制）
        bool enable_fps_limit;      ///< 是否启用FPS限制
        int skip_frames;            ///< 跳帧处理（每N帧处理一次）
        bool enable_async_processing; ///< 是否启用异步处理
        bool cache_features;        ///< 是否缓存特征文件

        // 时间统计
        mutable std::chrono::steady_clock::time_point last_frame_time;
        mutable int frame_count;
        mutable double average_fps;

        CoreConfig()
            : camera(0)
            , liveness(true)
            , face_threshold(0.62f)
            , liveness_threshold(0.8f)
            , debug(false)
            , target_fps(30)
            , enable_fps_limit(true)
            , skip_frames(1)
            , enable_async_processing(false)
            , cache_features(true)
            , last_frame_time(std::chrono::steady_clock::now())
            , frame_count(0)
            , average_fps(0.0) {}

        // 更新FPS统计
        void update_fps_statistics() const {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_frame_time).count();

            frame_count++;

            // 每100帧更新一次平均FPS
            if (frame_count >= 100) {
                if (elapsed > 0) {
                    average_fps = frame_count * 1000.0 / elapsed;
                }
                frame_count = 0;
                last_frame_time = now;
            }
        }

        // 获取当前FPS
        double get_current_fps() const {
            return average_fps;
        }

        // 计算帧间隔（毫秒）
        int get_frame_interval_ms() const {
            if (target_fps <= 0) return 0;
            return 1000 / target_fps;
        }

        // 检查是否需要处理当前帧（基于跳帧设置）
        bool should_process_frame(int frame_index) const {
            return (skip_frames <= 1) || (frame_index % skip_frames == 0);
        }
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

// 实现部分
ConfigManager::ConfigManager(const std::string& filename)
    : m_filename(filename), m_ini(m_filename) {
    // 初始化默认值
    m_config.camera = 0;
    m_config.liveness = true;
    m_config.face_threshold = 0.62f;
    m_config.liveness_threshold = 0.8f;
    m_config.debug = false;

    // 性能优化配置默认值
    m_config.target_fps = 30;
    m_config.enable_fps_limit = true;
    m_config.skip_frames = 1;
    m_config.enable_async_processing = false;
    m_config.cache_features = true;

    // 时间统计初始化
    m_config.last_frame_time = std::chrono::steady_clock::now();
    m_config.frame_count = 0;
    m_config.average_fps = 0.0;
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

    // 加载性能优化配置项
    if (!core_section.isKeyExist("target_fps")) {
        std::cerr << "Warning: Key 'target_fps' not found in [core]. Using default value (" << m_config.target_fps << ")." << std::endl;
    } else {
        m_config.target_fps = core_section.toInt("target_fps");
    }

    if (!core_section.isKeyExist("enable_fps_limit")) {
        std::cerr << "Warning: Key 'enable_fps_limit' not found in [core]. Using default value (" << m_config.enable_fps_limit << ")." << std::endl;
    } else {
        std::string fps_limit_str = core_section.toString("enable_fps_limit");
        if (fps_limit_str == "true" || fps_limit_str == "True" || fps_limit_str == "TRUE" || fps_limit_str == "1") {
            m_config.enable_fps_limit = true;
        } else if (fps_limit_str == "false" || fps_limit_str == "False" || fps_limit_str == "FALSE" || fps_limit_str == "0") {
            m_config.enable_fps_limit = false;
        } else {
             std::cerr << "Warning: Invalid value for 'enable_fps_limit' ('" << fps_limit_str << "'). Using default value (" << m_config.enable_fps_limit << ")." << std::endl;
        }
    }

    if (!core_section.isKeyExist("skip_frames")) {
        std::cerr << "Warning: Key 'skip_frames' not found in [core]. Using default value (" << m_config.skip_frames << ")." << std::endl;
    } else {
        m_config.skip_frames = core_section.toInt("skip_frames");
    }

    if (!core_section.isKeyExist("enable_async_processing")) {
        std::cerr << "Warning: Key 'enable_async_processing' not found in [core]. Using default value (" << m_config.enable_async_processing << ")." << std::endl;
    } else {
        std::string async_str = core_section.toString("enable_async_processing");
        if (async_str == "true" || async_str == "True" || async_str == "TRUE" || async_str == "1") {
            m_config.enable_async_processing = true;
        } else if (async_str == "false" || async_str == "False" || async_str == "FALSE" || async_str == "0") {
            m_config.enable_async_processing = false;
        } else {
             std::cerr << "Warning: Invalid value for 'enable_async_processing' ('" << async_str << "'). Using default value (" << m_config.enable_async_processing << ")." << std::endl;
        }
    }

    if (!core_section.isKeyExist("cache_features")) {
        std::cerr << "Warning: Key 'cache_features' not found in [core]. Using default value (" << m_config.cache_features << ")." << std::endl;
    } else {
        std::string cache_str = core_section.toString("cache_features");
        if (cache_str == "true" || cache_str == "True" || cache_str == "TRUE" || cache_str == "1") {
            m_config.cache_features = true;
        } else if (cache_str == "false" || cache_str == "False" || cache_str == "FALSE" || cache_str == "0") {
            m_config.cache_features = false;
        } else {
             std::cerr << "Warning: Invalid value for 'cache_features' ('" << cache_str << "'). Using default value (" << m_config.cache_features << ")." << std::endl;
        }
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
    file << "target_fps=" << m_config.target_fps << "\n";
    file << "enable_fps_limit=" << (m_config.enable_fps_limit ? "true" : "false") << "\n";
    file << "skip_frames=" << m_config.skip_frames << "\n";
    file << "enable_async_processing=" << (m_config.enable_async_processing ? "true" : "false") << "\n";
    file << "cache_features=" << (m_config.cache_features ? "true" : "false") << "\n";

    file.close();
    std::cout << "Default config created: " << m_filename << std::endl;
    return true;
}

bool ConfigManager::saveConfig() const {
    // 直接写入配置文件
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
    file << "target_fps=" << m_config.target_fps << "\n";
    file << "enable_fps_limit=" << (m_config.enable_fps_limit ? "true" : "false") << "\n";
    file << "skip_frames=" << m_config.skip_frames << "\n";
    file << "enable_async_processing=" << (m_config.enable_async_processing ? "true" : "false") << "\n";
    file << "cache_features=" << (m_config.cache_features ? "true" : "false") << "\n";

    file.close();
    return true;
}