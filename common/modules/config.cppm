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
