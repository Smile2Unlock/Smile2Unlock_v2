#ifndef SU_PLATFORM_WINDOWS_PATHS_H_
#define SU_PLATFORM_WINDOWS_PATHS_H_

#include <filesystem>
#include <optional>
#include <string_view>

namespace su::windows {

// Read the process's native UTF-16 environment, never the CRT's ANSI copy.
std::optional<std::filesystem::path> environment_path(std::wstring_view name);
std::filesystem::path executable_path();
std::filesystem::path temporary_directory();
std::filesystem::path roaming_app_data();

}  // namespace su::windows

#endif
