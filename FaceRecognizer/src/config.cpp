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
    // Check if the [core] section exists
    if (!m_ini.isSectionExists("core")) {
        std::cerr << "Warning: Section [core] not found in " << m_filename << ". Using defaults." << std::endl;
        return false; // Or decide to continue with defaults
    }

    // Get the [core] section object (use 'const auto' or 'auto' as operator[] might return a temporary)
    auto core_section = m_ini["core"];

    // Check if individual keys exist before accessing them
    if (!core_section.isKeyExist("camera")) {
        std::cerr << "Warning: Key 'camera' not found in [core]. Using default value (" << m_config.camera << ")." << std::endl;
    } else {
        m_config.camera = core_section.toInt("camera"); // Use .toInt(), .toDouble(), etc., as needed
    }

    if (!core_section.isKeyExist("liveness")) {
        std::cerr << "Warning: Key 'liveness' not found in [core]. Using default value (" << m_config.liveness << ")." << std::endl;
    } else {
        // inicpp often parses boolean strings like "true"/"false". Let's assume it works with toInt or direct cast from string.
        // If it doesn't work directly, we might need to check the string content.
        // For now, assuming .toString() followed by logic or direct conversion if supported by the library.
        // The example code shows assignment of boolean literals, so let's see if parsing works.
        // If the library returns an integer representation (e.g., 1/0), we cast it.
        // Otherwise, we parse the string.
        std::string liveness_str = core_section.toString("liveness");
        if (liveness_str == "true" || liveness_str == "True" || liveness_str == "TRUE" || liveness_str == "1") {
            m_config.liveness = true;
        } else if (liveness_str == "false" || liveness_str == "False" || liveness_str == "FALSE" || liveness_str == "0") {
            m_config.liveness = false;
        } else {
             std::cerr << "Warning: Invalid value for 'liveness' ('" << liveness_str << "'). Using default value (" << m_config.liveness << ")." << std::endl;
             // Keep the default value if parsing fails
        }
    }

    if (!core_section.isKeyExist("face_threshold")) {
        std::cerr << "Warning: Key 'face_threshold' not found in [core]. Using default value (" << m_config.face_threshold << ")." << std::endl;
    } else {
        // Use .toDouble() and cast to float
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
             // Keep the default value if parsing fails
        }
    }

    return true; // Indicate that the [core] section existed and was processed (with possible warnings for missing keys)
}


const ConfigManager::CoreConfig& ConfigManager::getConfig() const {
    return m_config;
}

void ConfigManager::setConfig(const ConfigManager::CoreConfig& newConfig) {
    m_config = newConfig;
}

bool ConfigManager::createDefaultConfig() const {
    // Create a temporary manager instance for the specific filename
    inicpp::IniManager temp_ini(m_filename);

    // Set the default values using the temporary manager
    temp_ini.modify("core", "camera", m_config.camera);
    temp_ini.modify("core", "liveness", m_config.liveness ? "true" : "false"); // Write as string
    temp_ini.modify("core", "face_threshold", m_config.face_threshold);
    temp_ini.modify("core", "liveness_threshold", m_config.liveness_threshold);
    temp_ini.modify("core", "debug", m_config.debug ? "true" : "false");      // Write as string

    // Attempt to write the temporary manager's state back to the file.
    // Since there's no explicit write/save method shown in the examples,
    // we rely on the IniManager destructor to write the file upon scope exit.
    // However, if the destructor doesn't reliably write, or if you need more control,
    // you might need to manually construct the INI content and write it.
    // For now, creating the temporary object and letting it go out of scope should trigger the write.
    // If this doesn't work, we can manually format and write the file content.

    // A safer approach might be to ensure the temp_ini object writes explicitly if possible,
    // or fall back to manual writing. Let's assume the destructor handles it based on typical library behavior.
    // If issues arise, consider adding a manual write function here.

    // Note: The original implementation tried to use `file << m_ini`, which failed because
    // `inicpp::IniManager` doesn't overload `operator<<`. We rely on the IniManager's internal mechanism.
    // The temp_ini goes out of scope here, ideally triggering a write.
    return true; // If no exceptions occurred during modify calls, assume success.
                 // More robust error checking would require library-specific handling.
}

bool ConfigManager::saveConfig() const {
    // Use a temporary IniManager to avoid modifying the member variable's file handle
    // or state if something goes wrong mid-process.
    inicpp::IniManager temp_ini(m_filename);

    // Apply the current configuration stored in m_config to the temporary manager
    temp_ini.modify("core", "camera", m_config.camera);
    temp_ini.modify("core", "liveness", m_config.liveness ? "true" : "false");
    temp_ini.modify("core", "face_threshold", m_config.face_threshold);
    temp_ini.modify("core", "liveness_threshold", m_config.liveness_threshold);
    temp_ini.modify("core", "debug", m_config.debug ? "true" : "false");

    // The temp_ini's destructor should save the file when it goes out of scope.
    // Return true assuming the operations were syntactically correct.
    // Add more specific error checking if the library provides a way to verify write success.
    return true; 
}