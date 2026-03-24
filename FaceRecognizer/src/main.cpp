// main.cpp
// 人脸识别主程序

#include "managers/ipc/udp/udp_sender.h"
#include "models/shared_frame_ipc.h"
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

int captureImageToSharedMemory(int camera_index, const std::string& map_name) {
    if (map_name.empty()) {
        std::cerr << "[Capture] 共享内存名称不能为空" << std::endl;
        return -1;
    }

    const size_t mapping_size =
        sizeof(smile2unlock::SharedFrameHeader) + smile2unlock::SharedFrameHeader::MAX_IMAGE_BYTES;
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, map_name.c_str());
    if (mapping == nullptr) {
        std::cerr << "[Capture] 打开共享内存失败" << std::endl;
        return -1;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapping_size);
    if (view == nullptr) {
        CloseHandle(mapping);
        std::cerr << "[Capture] 映射共享内存失败" << std::endl;
        return -1;
    }

    auto* header = static_cast<smile2unlock::SharedFrameHeader*>(view);
    *header = smile2unlock::SharedFrameHeader{};

    CameraCapture cam(camera_index);
    if (!cam.IsInitialized()) {
        header->status_code = static_cast<int32_t>(smile2unlock::SharedFrameStatus::CAPTURE_FAILED);
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        throw FaceRecognition::CameraException("无法打开摄像头");
    }

    SeetaImageData image{};
    if (!cam.CaptureFrame(image) || image.data == nullptr || image.width <= 0 || image.height <= 0) {
        header->status_code = static_cast<int32_t>(smile2unlock::SharedFrameStatus::CAPTURE_FAILED);
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return -1;
    }

    const uint32_t image_bytes = static_cast<uint32_t>(image.width * image.height * image.channels);
    if (image_bytes == 0 || image_bytes > smile2unlock::SharedFrameHeader::MAX_IMAGE_BYTES) {
        delete[] image.data;
        header->status_code = static_cast<int32_t>(smile2unlock::SharedFrameStatus::INVALID_FRAME);
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return -1;
    }

    header->width = image.width;
    header->height = image.height;
    header->channels = image.channels;
    header->image_bytes = image_bytes;
    header->status_code = static_cast<int32_t>(smile2unlock::SharedFrameStatus::READY);

    auto* bytes = reinterpret_cast<unsigned char*>(header + 1);
    memcpy(bytes, image.data, image_bytes);

    delete[] image.data;
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return 0;
}

// 人脸识别函数
int recognizeFace(int camera_index, bool liveness_detection, 
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

                    if (debug) {
                        std::cout << "[Recognize] 检测到真人脸，识别结果交由 Smile2Unlock 编排层处理" << std::endl;
                    }

                    recognition_success = true;

                    if (g_udp_sender) {
                        g_udp_sender->send_status(RecognitionStatus::SUCCESS, "");
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
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

    if (!recognition_success && g_udp_sender) {
        g_udp_sender->send_status(RecognitionStatus::TIMEOUT, "");
    }
    
    std::cout << "人脸识别结束。" << std::endl;
    return 0;
}

int mainOptimized(int argc, char* argv[]) {
    cxxopts::Options options("FaceRecognizer",
        "人脸识别工具");
    
    options.add_options()
        ("h,help", "显示帮助信息")
        ("m,mode", "操作模式: recognize, capture-image, test",
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
        ("frame-map", "Shared memory mapping name for one-shot frame capture",
         cxxopts::value<std::string>()->default_value(""))
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
    std::string frame_map = result["frame-map"].as<std::string>();
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
    if (mode == "recognize") {
        std::cout << "=== 人脸识别模式 ===" << std::endl;
                
        return recognizeFace(config.camera,
                                     config.liveness, liveness_threshold,
                                     config.debug);
    }
    else if (mode == "capture-image") {
        std::cout << "=== 单帧抓拍模式 ===" << std::endl;
        return captureImageToSharedMemory(config.camera, frame_map);
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
