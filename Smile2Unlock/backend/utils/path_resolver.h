#pragma once

#include <string>
#include <filesystem>
#include <windows.h>

namespace smile2unlock {
namespace utils {

/**
 * @brief 路径解析器 - 统一管理所有资源路径
 * 
 * 设计理念：
 * 1. 开发环境：使用源码相对路径（避免复制）
 * 2. 发布版本：使用 EXE 相对路径（标准打包结构）
 * 3. 配置文件：允许用户自定义路径
 */
class PathResolver {
public:
    static PathResolver& GetInstance() {
        static PathResolver instance;
        return instance;
    }

    /**
     * @brief 获取 EXE 所在目录
     * @return EXE 目录绝对路径，例如 "D:\app\bin\"
     */
    std::string GetExecutableDirectory() const {
        char buffer[MAX_PATH];
        GetModuleFileNameA(nullptr, buffer, MAX_PATH);
        std::filesystem::path exe_path(buffer);
        return exe_path.parent_path().string();
    }

    /**
     * @brief 获取项目根目录（开发环境）
     * @return 如果在开发环境返回源码根目录，否则返回空
     */
    std::string GetProjectDirectory() const {
        auto exe_dir = std::filesystem::path(GetExecutableDirectory());
        
        // 检测是否在 xmake 构建目录下
        // 例如：D:\project\Smile2Unlock_v2\build\windows\x64\release
        if (exe_dir.string().find("\\build\\windows\\") != std::string::npos) {
            // 向上 4 级到达项目根目录
            auto project_root = exe_dir.parent_path()  // x64
                                       .parent_path()  // windows
                                       .parent_path()  // build
                                       .parent_path(); // 项目根
            return project_root.string();
        }
        
        return "";  // 不在开发环境
    }

    /**
     * @brief 智能解析资源路径
     * @param relative_path 相对路径，例如 "resources/models/face_detector.csta"
     * @return 绝对路径，优先级：EXE目录 > 项目目录
     */
    std::string Resolve(const std::string& relative_path) const {
        namespace fs = std::filesystem;
        
        // 优先级 1: EXE 目录（发布版本）
        auto exe_path = fs::path(GetExecutableDirectory()) / relative_path;
        if (fs::exists(exe_path)) {
            return exe_path.string();
        }
        
        // 优先级 2: 项目目录（开发环境）
        auto project_dir = GetProjectDirectory();
        if (!project_dir.empty()) {
            auto project_path = fs::path(project_dir) / "FaceRecognizer" / relative_path;
            if (fs::exists(project_path)) {
                return project_path.string();
            }
        }
        
        // 回退：返回 EXE 相对路径（即使不存在）
        return exe_path.string();
    }

    /**
     * @brief 获取模型文件路径
     */
    std::string GetModelPath(const std::string& model_name) const {
        return Resolve("resources/models/" + model_name);
    }

    /**
     * @brief 获取配置文件路径
     */
    std::string GetConfigPath(const std::string& config_name) const {
        return Resolve("config/" + config_name);
    }

private:
    PathResolver() = default;
};

} // namespace utils
} // namespace smile2unlock
