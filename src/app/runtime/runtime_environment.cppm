module;

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

export module su.app.runtime;

import std;

export namespace su::app::runtime {

/**
 * @brief 获取可执行文件所在目录
 * @return std::filesystem::path 可执行文件目录路径
 */
std::filesystem::path executable_dir();

/**
 * @brief 准备运行时工作目录
 * 
 * 将当前工作目录切换到可执行文件所在目录，
 * 以确保相对路径能正确定位资源文件。
 */
void prepare_runtime_working_directory();

}  // namespace su::app::runtime

namespace su::app::runtime {

/**
 * @brief 获取可执行文件所在目录
 * 
 * Windows: 使用 GetModuleFileNameA 获取模块路径
 * Linux: 读取 /proc/self/exe 符号链接
 * 
 * @return std::filesystem::path 可执行文件目录，失败则返回当前路径
 */
std::filesystem::path executable_dir() {
#if defined(_WIN32)
    std::array<char, MAX_PATH> buffer{};
    const auto length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return length == 0 ? std::filesystem::current_path() : std::filesystem::path(std::string_view(buffer.data(), length)).parent_path();
#else
    std::array<char, 4096> buffer{};
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
    return length <= 0 ? std::filesystem::current_path() : std::filesystem::path(std::string_view(buffer.data(), static_cast<std::size_t>(length))).parent_path();
#endif
}

/**
 * @brief 准备运行时工作目录
 * 
 * 将当前工作目录切换到可执行文件所在目录。
 */
void prepare_runtime_working_directory() {
    std::error_code error;
    const auto target_dir = executable_dir();
    if (!target_dir.empty()) {
        std::filesystem::current_path(target_dir, error);
    }
}

}  // namespace su::app::runtime
