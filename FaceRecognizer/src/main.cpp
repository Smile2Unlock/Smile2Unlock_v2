#include "camera.h"
#include "udp_sender.h"
// #include "clipp.h"
#include "crypto.h"
#include "seetaface.h"
#include "config.h"
#include <cxxopts.hpp>
#include <iostream>
#include <string>
#include <filesystem>  // 用于路径操作
#include <chrono>      // 用于时间控制
#include <memory>      // 用于std::unique_ptr
#include "input_hide.h"
#include "registryhelper.h"


// 全局UDP发送器 - 保持整个程序生命周期
std::unique_ptr<udp_sender> g_udp_sender;
// camera* cam = new camera();         // TODO: Initialize when needed
std::queue<cv::Mat> frames;

// 人脸注册函数 - 简化版
int registerFace(int camera_index, float face_threshold, bool debug) {
    // 初始化摄像头
    camera cam(camera_index);
    
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
    std::cout << "请按 'c' 键拍照，按 'q' 键退出" << std::endl;
    
    // 检查摄像头是否打开
    if (!cam.isOpened()) {
        std::cerr << "错误：无法打开摄像头" << std::endl;
        return -1;
    }
    
    // 循环捕获图像并提取特征
    int captureCount = 0;
    const int maxCaptures = 5;  // 每人最多采集5张照片
    
    while (captureCount < maxCaptures) {
        // 等待队列中有帧可用
        if (!frames.empty()) {
            // 获取队列中的最新帧（移除旧帧）
            cv::Mat latest_frame;
            while (!frames.empty()) {
                latest_frame = frames.front();
                frames.pop();
            }
            
            // 显示实时画面
            cv::imshow("Face Registration - Press 'c' to capture, 'q' to quit", latest_frame);
            
            int key = cv::waitKey(1) & 0xFF; // 使用waitKey(1)而不是waitKey(30)，以便快速响应键盘输入
            if (key == 'q') {
                break; // 退出循环
            } else if (key == 'c') {
                // 转换为SeetaFace所需的格式
                seeta::cv::ImageData img_data(latest_frame);

                
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
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    } else {
                        std::cout << "警告：未能提取到人脸特征，请调整位置和角度" << std::endl;
                    }
                } else {
                    std::cout << "警告：未检测到人脸，请确保脸部清晰可见" << std::endl;
                }
            }
        } else {
            // 如果队列为空，短暂休眠避免CPU占用过高
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // 清理资源
    cv::destroyAllWindows();
    cam.~camera();
    
    std::cout << "人脸录入完成！用户 " << username << " 的 " << captureCount << " 个人脸数据已保存。" << std::endl;
    return 0;
}

// 人脸识别函数 - 简化版
int recognizeFace(int camera_index,
                  float face_threshold,
                  bool liveness_detection,
                  float liveness_threshold,
                  bool debug) {
  // 初始化摄像头
  camera cam(camera_index);

  // 初始化人脸检测器
  seetaface recognizer;

  std::cout << "开始人脸识别，请面对摄像头..." << std::endl;
  std::cout << "按 'q' 键退出识别" << std::endl;

  // 检查摄像头是否打开
  if (!cam.isOpened()) {
    std::cerr << "错误：无法打开摄像头" << std::endl;
    return -1;
  }

  // 初始化UDP发送器
  if (!g_udp_sender) {
    g_udp_sender = std::make_unique<udp_sender>();
    std::cout << "UDP发送器已初始化" << std::endl;
  } else {
    std::cout << "UDP发送器已存在，重用现有实例" << std::endl;
  }

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

          // 从文件名中提取用户名（假设格式为 username_index.dat）
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
    std::cerr << "警告：无法访问特征数据库，错误：" << e.what() << std::endl;
  }

  if (registeredUsers.empty()) {
    std::cerr << "错误：没有找到已注册的用户数据" << std::endl;
    return -1;
  }

  std::cout << "检测到已注册的用户：" << std::endl;
  for (const auto& user : registeredUsers) {
    std::cout << "  - " << user << std::endl;
  }

  int used_time = 0;
  // 主识别循环
  while (true) {
    // 等待队列中有帧可用
    if (!frames.empty()) {
      // 获取队列中的最新帧（移除旧帧）
      cv::Mat latest_frame;
      while (!frames.empty()) {
        latest_frame = frames.front();
        frames.pop();
      }

      // 显示实时画面
      // cv::imshow("Face Recognition - Press 'q' to quit", latest_frame);

      int key = cv::waitKey(1) & 0xFF;  // 使用waitKey(1)以便快速响应键盘输入
      if (key == 'q') {
        break;  // 退出循环
      }
      if (udp_status_code == static_cast<int>(RecognitionStatus::SUCCESS)) {
        break;
      }
      if (used_time >= 10000) {
        break;
      }

      // 转换为SeetaFace所需的格式
      seeta::cv::ImageData img_data(latest_frame);

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
              std::cout << "检测到非真实人脸，跳过识别" << std::endl;
              continue;
            }
          }

          // 与已注册的特征进行比较
          bool recognized = false;
          for (const auto& feature_file : featureFiles) {
            try {
              // 加载已注册的特征
              auto registered_features = recognizer.loadfeat(feature_file);

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
                std::cout << "[DEBUG] 发送识别成功状态" << std::endl;

                // 发送解锁信号
                udp_status_code = static_cast<int>(RecognitionStatus::SUCCESS);
                if (g_udp_sender) {
                  g_udp_sender->send_status(RecognitionStatus::SUCCESS,
                                            username);
                }

                // 等待足够的时间让凭证程序接收状态
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::cout << "[DEBUG] 已发送识别成功信号" << std::endl;

                recognized = true;
                break;  // 找到匹配项，跳出循环
              }
            } catch (const std::exception& e) {
              std::cerr << "加载特征文件时出错: " << feature_file
                        << ", 错误: " << e.what() << std::endl;
              continue;
            }
          }

          if (!recognized) {
            std::cout << "未识别到已注册用户" << std::endl;
          }
        }
      }
    } else {
      // 如果队列为空，短暂休眠避免CPU占用过高
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      used_time += 10;
    }
  }

  // 清理资源
  cv::destroyAllWindows();
  cam.~camera();

  // 如果识别成功，保持UDP发送器运行一段时间以确保凭证程序接收
  if (udp_status_code == static_cast<int>(RecognitionStatus::SUCCESS)) {
    std::cout << "[INFO] 识别成功，保持UDP连接2秒以确保凭证程序接收"
              << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  }

  // 不要手动销毁g_udp_sender，让它的析构函数自动处理
  std::cout << "人脸识别结束。" << std::endl;
  return 0;
}

int main(int argc, char* argv[]) {
    cxxopts::Options options("FaceRecognizer", "Command line tool for face recognition and unlocking");
    
    options.add_options()
        ("h,help", "Print help")
        ("m,mode", "Operation mode: register, recognize, test", cxxopts::value<std::string>()->default_value("help"))
        ("c,camera", "Camera index", cxxopts::value<int>()->default_value("0"))
        ("t,threshold", "Threshold type and value: face=<value>, liveness=<value>", cxxopts::value<std::vector<std::string>>())
        ("d,debug", "Enable debug output", cxxopts::value<bool>()->default_value("false"))
        ("l,liveness-detection", "Enable liveness detection", cxxopts::value<bool>()->default_value("true"))
        ("s,set-password", "Set the login password interactively", cxxopts::value<bool>()->default_value("false"))
        ;

    auto result = options.parse(argc, argv);

    // Show help by default when no arguments provided or when mode is help
    if (argc == 1 || result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    std::string mode = result["mode"].as<std::string>();
    bool set_password = result["set-password"].as<bool>();


    //加载配置
    ConfigManager config_manager("config.ini");
    // 1. 尝试加载现有配置
    if (!config_manager.loadConfig()) {
        std::cout << "Failed to load existing config. Creating default..." << std::endl;
        // 2. 如果加载失败（例如文件不存在），创建默认配置
        if (config_manager.createDefaultConfig()) {
            std::cout << "Default config created successfully. Loading it now..." << std::endl;
            // 再次尝试加载刚创建的默认配置
            if (!config_manager.loadConfig()) {
                std::cerr << "Failed to load the newly created default config." << std::endl;
                return -1;
            }
        } else {
            std::cerr << "Failed to create default config." << std::endl;
            return -1;
        }
    }

    auto current_config = config_manager.getConfig();


    
    if (result.count("threshold")) {
      // Parse thresholds
      float face_threshold = 0.62f;     // default face recognition threshold
      float liveness_threshold = 0.8f;  // default liveness detection threshold
      auto thresholds = result["threshold"].as<std::vector<std::string>>();
      for (const auto& thresh : thresholds) {
        if (thresh.substr(0, 5) == "face=") {
          face_threshold = std::stof(thresh.substr(5));
          current_config.face_threshold = face_threshold;
          config_manager.setConfig(current_config);
          if (config_manager.saveConfig()) {
            std::cout << "\nModified config saved successfully!" << std::endl;
          } else {
            std::cerr << "\nFailed to save modified config." << std::endl;
          }
        } else if (thresh.substr(0, 8) == "liveness=") {
          liveness_threshold = std::stof(thresh.substr(8));
          current_config.liveness_threshold = liveness_threshold;
          config_manager.setConfig(current_config);
          if (config_manager.saveConfig()) {
            std::cout << "\nModified config saved successfully!" << std::endl;
          } else {
            std::cerr << "\nFailed to save modified config." << std::endl;
          }
        }
        }
        return 0;
    }
    auto camera_index = result["camera"].as<int>();

    if (result.count("camera")) {
        current_config.camera = camera_index;
        config_manager.setConfig(current_config);
        if (config_manager.saveConfig()) {
        std::cout << "\nModified config saved successfully!" << std::endl;
        } else {
        std::cerr << "\nFailed to save modified config." << std::endl;
        }
        return 0;
    }

    if (result.count("debug")) {
        auto debug = result["debug"].as<bool>();
        current_config.debug = debug;
        config_manager.setConfig(current_config);
        if (config_manager.saveConfig()) {
        std::cout << "\nModified config saved successfully!" << std::endl;
        } else {
        std::cerr << "\nFailed to save modified config." << std::endl;
        }
        return 0;
    }

    if (result.count("liveness")) {
        auto liveness_detection = result["liveness-detection"].as<bool>();
        current_config.liveness = liveness_detection;
        config_manager.setConfig(current_config);
        if (config_manager.saveConfig()) {
        std::cout << "\nModified config saved successfully!" << std::endl;
        } else {
        std::cerr << "\nFailed to save modified config." << std::endl;
        }
        return 0;
    }

    // 初始化UDP发送器
    if (!g_udp_sender) {
        g_udp_sender = std::make_unique<udp_sender>();
        std::cout << "[Main] 全局UDP发送器已初始化" << std::endl;
    }

    // 重置状态码
    udp_status_code = static_cast<int>(RecognitionStatus::IDLE);

    // 如果是 register 模式，自动将当前路径写入注册表
    if (mode == "register") {
        try {
            // 获取当前可执行文件的路径
            std::string exePath;
#ifdef _WIN32
            char buffer[MAX_PATH];
            GetModuleFileNameA(NULL, buffer, MAX_PATH);
            std::filesystem::path fullPath(buffer);
            exePath = fullPath.parent_path().string();
#else
            exePath = Utils::getCurrentDirectory();
#endif

            RegistryHelper registryHelper;
            if (registryHelper.WriteStringToRegistry(
                "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\path", exePath)) {
                std::cout << "[INFO] FaceRecognizer 路径已写入注册表: " << exePath << std::endl;
            } else {
                std::cerr << "[WARNING] 无法写入注册表路径，将使用本地查找" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[WARNING] 获取当前路径失败: " << e.what() << std::endl;
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
                reinterpret_cast<const CryptoPP::byte *>(ciphertext.data()),
                ciphertext.size()));

        // 保存密文到文件
        // std::ofstream ofs("password.enc", std::ios::binary);
        // ofs.write(ciphertext.data(), ciphertext.size());
        // ofs.close();

        // 保存密文密钥IV至注册表中

        RegistryHelper registryHelper;

        registryHelper.WriteStringToRegistry(
            "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\password", ciphertext_hex);
        
        registryHelper.WriteStringToRegistry(
            "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\iv", crypto.getIvHex());

        registryHelper.WriteStringToRegistry(
            "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\key", crypto.getKeyHex());

        // 输出密钥和IV（十六进制）
        std::cout << "Encryption Key (hex): " << crypto.getKeyHex() << std::endl;
        std::cout << "IV (hex): " << crypto.getIvHex() << std::endl;
        std::cout << "Ciphertext (hex): " << ciphertext_hex << std::endl;

        return 0; // Exit after setting password
    }
    
    if (mode == "register") {
        // 调用人脸注册函数
        return registerFace(current_config.camera, current_config.face_threshold, current_config.debug);
    }
    else if (mode == "recognize") {
        // 调用人脸识别函数
        return recognizeFace(current_config.camera, current_config.face_threshold, current_config.liveness, current_config.liveness_threshold, current_config.debug);
    }
    else if (mode == "test") {
        // TODO: Initialize camera when needed
        // camera* cam = new camera(camera_index);
        
        // TODO: Implement test mode
        // - Test camera functionality
        // - Test face detection
        // - Test face recognition pipeline
        // - Test liveness detection
        std::cout << "Test mode: Testing face recognition pipeline..." << std::endl;
    }
    else {
        std::cerr << "Error: Invalid mode. Use 'register', 'recognize', or 'test'." << std::endl;
        std::cout << options.help() << std::endl;
        return -1;
    }
    
    // TODO: Add proper cleanup and exit handling
    return 0;
}