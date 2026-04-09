module;

#include "utils/app_paths.h"

export module app_paths;

import std;

export namespace smile2unlock::paths {

inline std::filesystem::path GetExecutableDirectory(HMODULE module_handle = nullptr) {
    return ::smile2unlock::GetExecutableDirectory(module_handle);
}

inline std::filesystem::path GetResourcesDirectory(HMODULE module_handle = nullptr) {
    return ::smile2unlock::GetResourcesDirectory(module_handle);
}

inline std::filesystem::path GetRecognizerModelsDirectory(HMODULE module_handle = nullptr) {
    return ::smile2unlock::GetRecognizerModelsDirectory(module_handle);
}

inline std::filesystem::path GetInstallRoot(HMODULE module_handle = nullptr) {
    return ::smile2unlock::GetInstallRoot(module_handle);
}

inline std::filesystem::path GetConfigDirectory(HMODULE module_handle = nullptr) {
    return ::smile2unlock::GetConfigDirectory(module_handle);
}

inline std::filesystem::path GetDataDirectory(HMODULE module_handle = nullptr) {
    return ::smile2unlock::GetDataDirectory(module_handle);
}

inline std::filesystem::path GetFaceDataDirectory(HMODULE module_handle = nullptr) {
    return ::smile2unlock::GetFaceDataDirectory(module_handle);
}

inline std::filesystem::path GetConfigIniPath(HMODULE module_handle = nullptr) {
    return ::smile2unlock::GetConfigIniPath(module_handle);
}

inline std::filesystem::path GetImguiIniPath(HMODULE module_handle = nullptr) {
    return ::smile2unlock::GetImguiIniPath(module_handle);
}

inline std::filesystem::path GetDatabasePath(HMODULE module_handle = nullptr) {
    return ::smile2unlock::GetDatabasePath(module_handle);
}

inline std::filesystem::path GetDatabaseKeyPath(HMODULE module_handle = nullptr) {
    return ::smile2unlock::GetDatabaseKeyPath(module_handle);
}

inline void EnsureRuntimeDirectories(HMODULE module_handle = nullptr) {
    ::smile2unlock::EnsureRuntimeDirectories(module_handle);
}

inline void MoveFileIfExists(const std::filesystem::path& source,
                             const std::filesystem::path& destination) {
    ::smile2unlock::MoveFileIfExists(source, destination);
}

inline void MigrateLegacyDataFiles(HMODULE module_handle = nullptr) {
    ::smile2unlock::MigrateLegacyDataFiles(module_handle);
}

} // namespace smile2unlock::paths
