#pragma once

#include "registryhelper.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace smile2unlock {

inline std::filesystem::path GetExecutableDirectory(HMODULE module_handle = nullptr) {
    char module_path[MAX_PATH] = {};
    if (GetModuleFileNameA(module_handle, module_path, MAX_PATH) == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(module_path).parent_path();
}

inline std::filesystem::path GetResourcesDirectory(HMODULE module_handle = nullptr) {
    return GetExecutableDirectory(module_handle) / "resources";
}

inline std::filesystem::path GetRecognizerModelsDirectory(HMODULE module_handle = nullptr) {
    return GetResourcesDirectory(module_handle) / "models";
}

inline std::filesystem::path GetInstallRoot(HMODULE module_handle = nullptr) {
    const std::string registry_path = RegistryHelper::ReadStringFromRegistry(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\path", "");
    if (!registry_path.empty()) {
        return std::filesystem::path(registry_path);
    }
    return GetExecutableDirectory(module_handle);
}

inline std::filesystem::path GetConfigDirectory(HMODULE module_handle = nullptr) {
    return GetInstallRoot(module_handle) / "config";
}

inline std::filesystem::path GetDataDirectory(HMODULE module_handle = nullptr) {
    return GetInstallRoot(module_handle) / "data";
}

inline std::filesystem::path GetFaceDataDirectory(HMODULE module_handle = nullptr) {
    return GetDataDirectory(module_handle) / "face";
}

inline std::filesystem::path GetConfigIniPath(HMODULE module_handle = nullptr) {
    return GetConfigDirectory(module_handle) / "config.ini";
}

inline std::filesystem::path GetImguiIniPath(HMODULE module_handle = nullptr) {
    return GetConfigDirectory(module_handle) / "imgui.ini";
}

inline std::filesystem::path GetDatabasePath(HMODULE module_handle = nullptr) {
    return GetDataDirectory(module_handle) / "smile2unlock.db";
}

inline std::filesystem::path GetDatabaseKeyPath(HMODULE module_handle = nullptr) {
    return GetDataDirectory(module_handle) / "smile2unlock.key";
}

inline void EnsureRuntimeDirectories(HMODULE module_handle = nullptr) {
    std::error_code ec;
    std::filesystem::create_directories(GetConfigDirectory(module_handle), ec);
    ec.clear();
    std::filesystem::create_directories(GetDataDirectory(module_handle), ec);
    ec.clear();
    std::filesystem::create_directories(GetFaceDataDirectory(module_handle), ec);
}

inline void MoveFileIfExists(const std::filesystem::path& source,
                             const std::filesystem::path& destination) {
    std::error_code ec;
    if (source.empty() || destination.empty() || source == destination) {
        return;
    }
    if (!std::filesystem::exists(source, ec) || ec) {
        return;
    }
    ec.clear();
    if (std::filesystem::exists(destination, ec) || ec) {
        return;
    }

    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), ec);
        ec.clear();
    }

    std::filesystem::rename(source, destination, ec);
    if (!ec) {
        return;
    }

    ec.clear();
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, ec);
    if (ec) {
        return;
    }
    ec.clear();
    std::filesystem::remove(source, ec);
}

inline void MigrateLegacyDataFiles(HMODULE module_handle = nullptr) {
    EnsureRuntimeDirectories(module_handle);

    const std::filesystem::path root = GetInstallRoot(module_handle);
    MoveFileIfExists(root / "smile2unlock.db", GetDatabasePath(module_handle));
    MoveFileIfExists(root / "smile2unlock.key", GetDatabaseKeyPath(module_handle));
}

} // namespace smile2unlock
