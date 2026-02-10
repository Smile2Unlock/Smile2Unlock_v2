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

    file.close();
    return true;
}