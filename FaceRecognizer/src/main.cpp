// main_optimized.cpp
// 性能优化版本：添加 FPS 控制、特征缓存和跳帧处理

#include "managers/ipc/udp/udp_sender.h"
#include "crypto.h"
#include "seetaface.h"
#include "fast_detector.h"  // 轻量级检测器
#include "config.h"
#include "exceptions.h"
#include <cxxopts.hpp>
#include <iostream>
#include <string>
#include <filesystem>  // 用于路径操作
#include <chrono>      // 用于时间控制
#include <memory>      // 用于std::unique_ptr
#include <thread>      // 用于sleep
#include <atomic>      // 用于线程安全
#include <vector>
#include <algorithm>
#include <mutex>
#include "input_hide.h"
#include "registryhelper.h"

import camera;
import utils;

// 全局UDP发送器 - 使用 Boost.Asio 实现
std::unique_ptr<UdpSender> g_udp_sender;

// 简单的FPS控制器
class SimpleFPSController {
private:
    int target_fps_;
    std::chrono::steady_clock::time_point last_frame_time_;
    std::chrono::steady_clock::time_point next_frame_time_;
    std::atomic<int> actual_fps_{0};
    std::atomic<int> frame_count_{0};
    std::chrono::steady_clock::time_point fps_start_time_;
    
public:
    SimpleFPSController(int target_fps = 30) 
        : target_fps_(target_fps)
        , last_frame_time_(std::chrono::steady_clock::now())
        , next_frame_time_(last_frame_time_)
        , fps_start_time_(last_frame_time_) {
        
        if (target_fps_ <= 0) target_fps_ = 30;
    }
    
    void set_target_fps(int fps) {
        if (fps > 0 && fps <= 120) {
            target_fps_ = fps;
        }
    }
    
    // 等待下一帧（FPS限制）
    void wait_for_next_frame() {
        if (target_fps_ <= 0) return;
        
        auto now = std::chrono::steady_clock::now();
        int frame_interval_ms = 1000 / target_fps_;
        
        // 计算下一帧时间
        next_frame_time_ += std::chrono::milliseconds(frame_interval_ms);
        
        // 如果当前时间已经超过下一帧时间，立即返回
        if (now >= next_frame_time_) {
            next_frame_time_ = now;
            return;
        }
        
        // 等待到下一帧时间
        std::this_thread::sleep_until(next_frame_time_);
    }
    
    // 更新FPS统计
    void update_frame() {
        frame_count_++;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - fps_start_time_).count();
        
        // 每秒更新一次实际FPS
        if (elapsed >= 1000) {
            actual_fps_ = static_cast<int>(frame_count_ * 1000.0 / elapsed);
            frame_count_ = 0;
            fps_start_time_ = now;
            
            // 输出FPS信息（调试用）
            // std::cout << "[FPS] 实际FPS: " << actual_fps_ << ", 目标FPS: " << target_fps_ << std::endl;
        }
    }
    
    int get_actual_fps() const {
        return actual_fps_;
    }
};

// 特征缓存管理器
class FeatureCache {
private:
    struct CachedFeature {
        std::string filename;
        std::shared_ptr<float> features;
        std::chrono::steady_clock::time_point load_time;
    };
    
    std::vector<CachedFeature> cache_;
    mutable std::mutex mutex_;
    size_t max_cache_size_{100};
    
public:
    FeatureCache(size_t max_size = 100) : max_cache_size_(max_size) {}
    
    std::shared_ptr<float> get_or_load(const std::string& filename, seetaface& recognizer) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 查找缓存
        auto it = std::find_if(cache_.begin(), cache_.end(),
            [&filename](const CachedFeature& cf) { return cf.filename == filename; });
        
        if (it != cache_.end()) {
            // 更新访问时间
            it->load_time = std::chrono::steady_clock::now();
            return it->features;
        }
        
        // 加载特征
        try {
            auto features = recognizer.loadfeat(filename);
            if (features) {
                // 添加到缓存
                if (cache_.size() >= max_cache_size_) {
                    // 移除最旧的缓存项
                    auto oldest = std::min_element(cache_.begin(), cache_.end(),
                        [](const CachedFeature& a, const CachedFeature& b) {
                            return a.load_time < b.load_time;
                        });
                    if (oldest != cache_.end()) {
                        cache_.erase(oldest);
                    }
                }
                
                cache_.push_back({filename, features, std::chrono::steady_clock::now()});
                return features;
            }
        } catch (const std::exception& e) {
            std::cerr << "加载特征文件失败: " << filename << " - " << e.what() << std::endl;
        }
        
        return nullptr;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        return cache_.size();
    }
};

// 人脸注册函数 - 优化版
int registerFaceOptimized(int camera_index, float face_threshold, bool debug, int target_fps = 30) {
    // 初始化摄像头
    CameraCapture cam(camera_index);
    
    // 获取用户名
    std::string username;
    std::cout << "请输入用户名: ";
    std::cin >> username;
    
    // 验证用户名的有效性
    if (username.empty()) {
        std::cerr << "错误：用户名不能为空" << std::endl;
        return -1;
    }
    
    // 初始化人脸检测器
    seetaface recognizer;
    
    std::cout << "开始录入 " << username << " 的人脸数据，请面对摄像头..." << std::endl;
    std::cout << "目标FPS: " << target_fps << std::endl;
    
    // 检查摄像头是否打开
    if (!cam.IsInitialized()) {
        throw FaceRecognition::CameraException("无法打开摄像头");
    }
    
    // FPS控制器
    SimpleFPSController fps_controller(target_fps);
    
    // 循环捕获图像并提取特征
    int captureCount = 0;
    const int maxCaptures = 5;  // 每人最多采集5张照片
    int frame_counter = 0;
    
    while (captureCount < maxCaptures) {
        fps_controller.wait_for_next_frame();
        
        // 从摄像头直接捕获帧
        SeetaImageData img_data = {};
        if (cam.CaptureFrame(img_data)) {
            frame_counter++;
            
            // 每2帧处理一次（减少处理负载）
            if (frame_counter % 2 != 0) {
                delete[] img_data.data;
                continue;
            }
            
            // 检测人脸
            SeetaRect face_rect = recognizer.detect(img_data);
            
            if (face_rect.width > 0 && face_rect.height > 0) {
                // 检测到人脸，提取特征
                auto features = recognizer.img2features(img_data);
                
                if (features) {
                    // 保存特征到指定文件
                    std::string feature_file = username + "_" + std::to_string(captureCount) + ".dat";
                    recognizer.savefeat(features, feature_file);
                    
                    std::cout << "已保存第 " << captureCount + 1 << " 张人脸特征: " << feature_file << std::endl;
                    captureCount++;
                    
                    // 短暂延迟，避免连续捕获
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                } else {
                    std::cout << "警告：未能提取到人脸特征，请调整位置和角度" << std::endl;
                }
            } else {
                std::cout << "警告：未检测到人脸，请确保脸部清晰可见" << std::endl;
            }
            
            // 释放当前帧的内存
            delete[] img_data.data;
            
            // 更新FPS统计
            fps_controller.update_frame();
        } else {
            // 如果捕获失败，短暂休眠避免CPU占用过高
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    std::cout << "人脸录入完成！用户 " << username << " 的 " << captureCount << " 个人脸数据已保存。" << std::endl;
    std::cout << "平均FPS: " << fps_controller.get_actual_fps() << std::endl;
    return 0;
}

// 人脸识别函数 - 优化版
int recognizeFaceOptimized(int camera_index, float face_threshold, bool liveness_detection, 
                          float liveness_threshold, bool debug, int target_fps = 30, 
                          int skip_frames = 2, bool enable_cache = true) {
    
    std::cout << "[Recognize] 启动人脸识别模式（优化版）..." << std::endl;
    std::cout << "[配置] 目标FPS: " << target_fps << ", 跳帧: " << skip_frames 
              << ", 特征缓存: " << (enable_cache ? "启用" : "禁用") << std::endl;
    
    // 识别结果标志
    bool recognition_success = false;
    
    // 初始化UDP发送器
    if (!g_udp_sender) {
        g_udp_sender = std::make_unique<UdpSender>();
    }
    
    // 发送 RECOGNIZING 状态
    if (g_udp_sender) {
        g_udp_sender->send_status(RecognitionStatus::RECOGNIZING, "");
    }
    
    std::cout << "[Recognize] [1/3] 初始化摄像头..." << std::endl;
    CameraCapture cam(camera_index);
    
    // 检查摄像头是否打开
    if (!cam.IsInitialized()) {
        throw FaceRecognition::CameraException("无法打开摄像头");
    }
    std::cout << "[Recognize] [1/3] 摄像头初始化完成" << std::endl;
    
    std::cout << "[Recognize] [2/3] 加载人脸识别模型（耗时较长，请稍候）..." << std::endl;
    seetaface recognizer;
    std::cout << "[Recognize] [2/3] 模型加载完成" << std::endl;
    
    // 特征缓存
    FeatureCache feature_cache(50);
    
    // 查找已注册的用户特征文件
    std::vector<std::string> registeredUsers;
    std::vector<std::string> featureFiles;
    
    try {
        std::string currentDir = Utils::getCurrentDirectory();
        std::filesystem::path faceDir =
            std::filesystem::path(currentDir) / "data" / "face";
        
        if (std::filesystem::exists(faceDir) &&
            std::filesystem::is_directory(faceDir)) {
            for (const auto& entry : std::filesystem::directory_iterator(faceDir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".dat") {
                    featureFiles.push_back(entry.path().filename().string());
                    
                    // 从文件名中提取用户名
                    std::string filename = entry.path().stem().string();
                    size_t underscore_pos = filename.find('_');
                    if (underscore_pos != std::string::npos) {
                        std::string username = filename.substr(0, underscore_pos);
                        if (std::find(registeredUsers.begin(), registeredUsers.end(),
                                      username) == registeredUsers.end()) {
                            registeredUsers.push_back(username);
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        throw FaceRecognition::FileOperationException("特征数据库", e.what());
    }
    
    if (registeredUsers.empty()) {
        throw FaceRecognition::FileOperationException("特征数据库", "没有找到已注册的用户数据");
    }
    
    std::cout << "检测到已注册的用户：" << std::endl;
    for (const auto& user : registeredUsers) {
        std::cout << "  - " << user << std::endl;
    }
    std::cout << "特征文件数量: " << featureFiles.size() << std::endl;
    
    // 性能优化组件
    SimpleFPSController fps_controller(target_fps);
    int used_time = 0;
    int frame_counter = 0;
    int processed_frames = 0;
    
    std::cout << "[Recognize] [3/3] 准备就绪，开始识别..." << std::endl;
    
    // 主识别循环 - 优化版
    while (true) {
        // FPS控制
        fps_controller.wait_for_next_frame();
        
        // 从摄像头捕获帧
        SeetaImageData img_data = {};
        if (cam.CaptureFrame(img_data)) {
            frame_counter++;
            
            // 跳帧处理：每skip_frames帧处理一次
            if (skip_frames > 1 && frame_counter % skip_frames != 0) {
                delete[] img_data.data;
                fps_controller.update_frame();
                continue;
            }
            
            processed_frames++;
            
            // 检查识别成功或超时
            if (recognition_success) {
                delete[] img_data.data;
                break;
            }
            if (used_time >= 10000) {  // 10秒超时
                delete[] img_data.data;
                break;
            }
            
            // 检测人脸
            SeetaRect face_rect = recognizer.detect(img_data);
            
            if (face_rect.width > 0 && face_rect.height > 0) {
                // 检测到人脸，提取特征
                auto current_features = recognizer.img2features(img_data);
                
                if (current_features) {
                    // 如果启用了活体检测，验证是否为真实人脸
                    if (liveness_detection) {
                        bool is_real = recognizer.anti_face(img_data, liveness_threshold);
                        if (!is_real) {
                            if (debug) {
                                std::cout << "检测到非真实人脸，跳过识别" << std::endl;
                            }
                            delete[] img_data.data;
                            continue;
                        }
                    }
                    
                    // 与已注册的特征进行比较
                    bool recognized = false;
                    for (const auto& feature_file : featureFiles) {
                        try {
                            // 加载已注册的特征（使用缓存）
                            std::shared_ptr<float> registered_features;
                            
                            if (enable_cache) {
                                registered_features = feature_cache.get_or_load(feature_file, recognizer);
                            } else {
                                registered_features = recognizer.loadfeat(feature_file);
                            }
                            
                            if (!registered_features) {
                                continue;
                            }
                            
                            // 计算相似度
                            float similarity = recognizer.feat_compare(current_features,
                                                                      registered_features);
                            
                            // 如果相似度超过阈值，认为匹配成功
                            if (similarity > face_threshold) {
                                // 从文件名中提取用户名
                                std::string filename =
                                    std::filesystem::path(feature_file).stem().string();
                                size_t underscore_pos = filename.find('_');
                                std::string username = (underscore_pos != std::string::npos)
                                                           ? filename.substr(0, underscore_pos)
                                                           : "Unknown";
                                
                                std::cout << "识别成功！用户: " << username
                                          << ", 相似度: " << similarity << std::endl;
                                
                                // 标记识别成功
                                recognition_success = true;
                                
                                // 发送解锁信号
                                if (g_udp_sender) {
                                    g_udp_sender->send_status(RecognitionStatus::SUCCESS,
                                                              username);
                                }
                                
                                // 等待足够的时间让凭证程序接收状态
                                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                                
                                recognized = true;
                                break;  // 找到匹配项，跳出循环
                            }
                        } catch (const std::exception& e) {
                            if (debug) {
                                std::cerr << "加载特征文件时出错: " << feature_file
                                          << ", 错误: " << e.what() << std::endl;
                            }
                            continue;
                        }
                    }
                    
                    if (!recognized && debug) {
                        std::cout << "未识别到已注册用户" << std::endl;
                    }
                }
            }
            
            // 释放当前帧的内存
            delete[] img_data.data;
            
            // 更新FPS统计
            fps_controller.update_frame();
            
            // 每100帧输出一次性能信息
            if (debug && processed_frames % 100 == 0) {
                std::cout << "[性能] 处理帧数: " << processed_frames 
                          << ", 实际FPS: " << fps_controller.get_actual_fps()
                          << ", 缓存大小: " << feature_cache.size() << std::endl;
            }
            
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            used_time += 10;
        }
    }
    
    if (recognition_success) {
        std::cout << "[INFO] 识别成功，保持UDP连接0.1秒以确保凭证程序接收" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // 输出性能统计
    std::cout << "\n=== 性能统计 ===" << std::endl;
    std::cout << "总处理帧数: " << processed_frames << std::endl;
    std::cout << "平均FPS: " << fps_controller.get_actual_fps() << std::endl;
    std::cout << "特征缓存命中率: " << feature_cache.size() << " 个特征已缓存" << std::endl;
    std::cout << "识别结果: " << (recognition_success ? "成功" : "超时/失败") << std::endl;
    
    std::cout << "人脸识别结束。" << std::endl;
    return 0;
}

// 主函数 - 添加性能优化选项
int mainOptimized(int argc, char* argv[]) {
    cxxopts::Options options("FaceRecognizerOptimized",
        "优化版人脸识别工具 - 支持FPS限制和性能优化");
    
    options.add_options()
        ("h,help", "显示帮助信息")
        ("m,mode", "操作模式: register, recognize, test",
         cxxopts::value<std::string>()->default_value("help"))
        ("c,camera", "摄像头索引",
         cxxopts::value<int>()->default_value("0"))
        ("t,threshold", "阈值类型和值: face=<value>, liveness=<value>",
         cxxopts::value<std::vector<std::string>>())
        ("d,debug", "启用调试输出",
         cxxopts::value<bool>()->default_value("false"))
        ("l,liveness-detection", "启用活体检测",
         cxxopts::value<bool>()->default_value("true"))
        ("s,set-password", "交互式设置登录密码",
         cxxopts::value<bool>()->default_value("false"))
        // 性能优化选项
        ("f,fps", "目标FPS (默认: 30)",
         cxxopts::value<int>()->default_value("30"))
        ("skip-frames", "跳帧处理 (每N帧处理一次, 默认: 2)",
         cxxopts::value<int>()->default_value("2"))
        ("enable-cache", "启用特征缓存",
         cxxopts::value<bool>()->default_value("true"))
        ("disable-fps-limit", "禁用FPS限制",
         cxxopts::value<bool>()->default_value("false"))
        ;
    
    auto result = options.parse(argc, argv);
    
    // 显示帮助
    if (argc == 1 || result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }
    
    std::string mode = result["mode"].as<std::string>();
    bool set_password = result["set-password"].as<bool>();
    
    // 加载配置
    ConfigManager config_manager("config.ini");
    // 1. 尝试加载现有配置
        if (!config_manager.loadConfig()) {
            std::cout << "无法加载配置文件，创建默认配置..." << std::endl;
            // 2. 如果加载失败（例如文件不存在），创建默认配置
            if (config_manager.createDefaultConfig()) {
                std::cout << "默认配置文件创建成功，正在加载..." << std::endl;
                // 再次尝试加载刚创建的默认配置
                if (!config_manager.loadConfig()) {
                    throw FaceRecognition::ConfigException("无法加载新创建的默认配置文件");
                }
            } else {
                throw FaceRecognition::ConfigException("无法创建默认配置文件");
            }
        }
    
    auto config = config_manager.getConfig();
    
    // 解析阈值参数
    float face_threshold = config.face_threshold;
    float liveness_threshold = config.liveness_threshold;
    
    if (result.count("threshold")) {
        auto thresholds = result["threshold"].as<std::vector<std::string>>();
        for (const auto& t : thresholds) {
            if (t.find("face=") == 0) {
                face_threshold = std::stof(t.substr(5));
            } else if (t.find("liveness=") == 0) {
                liveness_threshold = std::stof(t.substr(9));
            }
        }
    }
    
    // 获取性能优化参数
    int target_fps = result["fps"].as<int>();
    int skip_frames = result["skip-frames"].as<int>();
    bool enable_cache = result["enable-cache"].as<bool>();
    bool disable_fps_limit = result["disable-fps-limit"].as<bool>();
    
    if (disable_fps_limit) {
        target_fps = 0; // 0表示禁用FPS限制
    }
    
    // 根据模式执行相应操作
    if (mode == "register") {
        std::cout << "=== 人脸注册模式 (优化版) ===" << std::endl;
        return registerFaceOptimized(config.camera, face_threshold,
                                     config.debug, target_fps);
    }
    else if (mode == "recognize") {
        std::cout << "=== 人脸识别模式 (优化版) ===" << std::endl;
        std::cout << "性能优化配置:" << std::endl;
        std::cout << "  - 目标FPS: " << (target_fps > 0 ? std::to_string(target_fps) : "无限制") << std::endl;
        std::cout << "  - 跳帧: " << skip_frames << std::endl;
        std::cout << "  - 特征缓存: " << (enable_cache ? "启用" : "禁用") << std::endl;
        
        return recognizeFaceOptimized(config.camera, face_threshold,
                                     config.liveness, liveness_threshold,
                                     config.debug, target_fps, skip_frames,
                                     enable_cache);
    }
    else if (mode == "test") {
        std::cout << "测试模式暂未实现" << std::endl;
        return 0;
    }
    else {
        std::cout << "未知模式: " << mode << std::endl;
        std::cout << options.help() << std::endl;
        return 1;
    }
    
    return 0;
}

// 包装函数，保持与原始main函数的兼容性
int main(int argc, char* argv[]) {
    #ifdef _WIN32
    SetConsoleOutputCP(65001);
    #endif
    try {
        return mainOptimized(argc, argv);
    } catch (const FaceRecognition::FaceRecognitionException& e) {
        std::cerr << "程序异常: " << e.what() << std::endl;
        return -1;
    } catch (const std::exception& e) {
        std::cerr << "程序异常: " << e.what() << std::endl;
        return -1;
    }
}