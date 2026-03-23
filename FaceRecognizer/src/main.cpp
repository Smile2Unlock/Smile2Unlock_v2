// main.cpp
// 人脸识别主程序

#include "managers/ipc/udp/udp_sender.h"
#include "seetaface.h"
#include "exceptions.h"
#include <cxxopts.hpp>
#include "registryhelper.h"

import camera;
import utils;
import config;
import std;
import crypto;
import input_hide;


// 全局 UDP 发送器 - 使用 Boost.Asio 实现
std::unique_ptr<UdpSender> g_udp_sender;
int g_udp_port = 51234;

// 人脸注册函数
int registerFace(int camera_index, float face_threshold, bool debug) {
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
    
    // 检查摄像头是否打开
    if (!cam.IsInitialized()) {
        throw FaceRecognition::CameraException("无法打开摄像头");
    }
    
    // 循环捕获图像并提取特征
    int captureCount = 0;
    const int maxCaptures = 5;  // 每人最多采集 5 张照片
        
    while (captureCount < maxCaptures) {
        
        // 从摄像头直接捕获帧
        SeetaImageData img_data = {};
        if (cam.CaptureFrame(img_data)) {
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
        } else {
            // 如果捕获失败，短暂休眠避免CPU占用过高
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    std::cout << "人脸录入完成！用户 " << username << " 的 " << captureCount << " 个人脸数据已保存。" << std::endl;
    return 0;
}

// 人脸识别函数
int recognizeFace(int camera_index, float face_threshold, bool liveness_detection, 
                  float liveness_threshold, bool debug) {
    
    std::cout << "[Recognize] 启动人脸识别模式" << std::endl;
    
    // 识别结果标志
    bool recognition_success = false;
    
    // 初始化UDP发送器
    if (!g_udp_sender) {
        g_udp_sender = std::make_unique<UdpSender>("127.0.0.1", g_udp_port);
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
        std::cout << "[Recognize] 未找到本地已注册特征，将继续运行并等待外部流程接管比对。" << std::endl;
    }
    
    std::cout << "检测到已注册的用户：" << std::endl;
    for (const auto& user : registeredUsers) {
        std::cout << "  - " << user << std::endl;
    }
    std::cout << "特征文件数量：" << featureFiles.size() << std::endl;
        
    int used_time = 0;
        
    // 主识别循环
    while (true) {
        // 从摄像头捕获帧
        SeetaImageData img_data = {};
        if (cam.CaptureFrame(img_data)) {
            
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
                    if (featureFiles.empty()) {
                        if (debug) {
                            std::cout << "[Recognize] 当前没有可用于比对的本地特征文件。" << std::endl;
                        }
                    } else {
                        for (const auto& feature_file : featureFiles) {
                            try {
                                // 加载已注册的特征
                                auto registered_features = recognizer.loadfeat(feature_file);
                                
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
            }
            
            // 释放当前帧的内存
            delete[] img_data.data;
            
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            used_time += 10;
        }
    }
    
    // 输出识别结果
    std::cout << "识别结果：" << (recognition_success ? "成功" : "超时/失败") << std::endl;
    
    std::cout << "人脸识别结束。" << std::endl;
    return 0;
}

int mainOptimized(int argc, char* argv[]) {
    cxxopts::Options options("FaceRecognizer",
        "人脸识别工具");
    
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
        ("udp-port", "UDP target port for sending recognition results",
         cxxopts::value<int>()->default_value("51234"))
        ("s,set-password", "交互式设置登录密码",
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
    g_udp_port = result["udp-port"].as<int>();
    
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
    
    
    // Handle set-password functionality separately
    if (set_password) {
      std::string password1, password2;

      password1 = HiddenInputReader{}.readHiddenLine("Enter new password: ");

      password2 = HiddenInputReader{}.readHiddenLine("Confirm new password: ");

      if (password1 != password2) {
        std::cerr << "Error: Passwords do not match!" << std::endl;
        return -1;
      }

      // TODO: Implement set password functionality
      // - Encrypt and securely store the password
      // - Potentially integrate with Windows credential manager
      std::cout << "Set password: Setting login password..." << std::endl;

      Crypto crypto;

      // 自动生成密钥和IV（构造函数已实现）
      std::string ciphertext = crypto.encrypt(password1);

      std::string ciphertext_hex =
          Crypto::bytesToHex(std::span<const CryptoPP::byte>(
              reinterpret_cast<const CryptoPP::byte*>(ciphertext.data()),
              ciphertext.size()));

      // 保存密文到文件
      // std::ofstream ofs("password.enc", std::ios::binary);
      // ofs.write(ciphertext.data(), ciphertext.size());
      // ofs.close();

      // 保存密文密钥IV至注册表中

      RegistryHelper registryHelper;

      registryHelper.WriteStringToRegistry(
          "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\password",
          ciphertext_hex);

      registryHelper.WriteStringToRegistry(
          "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\iv",
          crypto.getIvHex());

      registryHelper.WriteStringToRegistry(
          "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\key",
          crypto.getKeyHex());

      // 输出密钥和IV（十六进制）
      std::cout << "Encryption Key (hex): " << crypto.getKeyHex() << std::endl;
      std::cout << "IV (hex): " << crypto.getIvHex() << std::endl;
      std::cout << "Ciphertext (hex): " << ciphertext_hex << std::endl;

      return 0;  // Exit after setting password
    }

    if (!g_udp_sender) {
        g_udp_sender = std::make_unique<UdpSender>("127.0.0.1", g_udp_port);
    }

    // 根据模式执行相应操作
    if (mode == "register") {

        try {
            std::string exePath;
            exePath = Utils::getCurrentDirectory();
            RegistryHelper registryHelper;
            if (registryHelper.WriteStringToRegistry(
            "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\path", exePath)) {
                std::cout << "[INFO] FaceRecognizer 路径已写入注册表: " << exePath << std::endl;
            } else {
                throw FaceRecognition::RegistryException("Write", "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\path", "Access denied");
            }
        } catch (const std::exception& e) {
            throw FaceRecognition::PathOperationException("Failed to get current directory");
        }

        std::cout << "=== 人脸注册模式 ===" << std::endl;
        return registerFace(config.camera, face_threshold,
                                     config.debug);
    }
    else if (mode == "recognize") {
        std::cout << "=== 人脸识别模式 ===" << std::endl;
                
        return recognizeFace(config.camera, face_threshold,
                                     config.liveness, liveness_threshold,
                                     config.debug);
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