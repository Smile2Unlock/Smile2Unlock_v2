#include "Config.h"
#include <iostream>
#include <fstream> // Required for std::ofstream

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