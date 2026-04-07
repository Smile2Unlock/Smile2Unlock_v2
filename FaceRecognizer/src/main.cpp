// main.cpp
// 人脸识别主程序

#include "managers/ipc/udp/udp_sender.h"
#include "models/shared_frame_ipc.h"
#include "seetaface.h"
#include "exceptions.h"
#include "utils/logger.h"
#include <cxxopts.hpp>
#include "registryhelper.h"

import camera;
import app_paths;
import utils;
import config;
import std;
import crypto;


// 全局 UDP 发送器 - 使用 Boost.Asio 实现
std::unique_ptr<UdpSender> g_udp_sender;
int g_udp_port = 51234;

namespace {

void InitializeProcessLogging() {
    smile2unlock::paths::EnsureRuntimeDirectories();
    smile2unlock::paths::MigrateLegacyDataFiles();
    smile2unlock::ConfigureProcessFileLogging(
        "FR", smile2unlock::ResolveLogDirectoryFromModule(nullptr));
    smile2unlock::InstallStandardStreamFileLogging();
    smile2unlock::WriteFileLogLine("BOOT", "FaceRecognizer process logging initialized");
}

std::string EncodeBytesToHex(const unsigned char* data, size_t size) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.resize(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result[i * 2] = kHex[(data[i] >> 4) & 0x0F];
        result[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return result;
}

std::vector<unsigned char> DecodeHexToBytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("Hex string length must be even");
    }

    auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
    };

    std::vector<unsigned char> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int high = hex_value(hex[i]);
        const int low = hex_value(hex[i + 1]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("Invalid hex character");
        }
        bytes.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return bytes;
}

} // namespace

int captureImageToSharedMemory(int camera_index, const std::string& map_name, bool extract_feature) {
    if (map_name.empty()) {
        std::cerr << "[Capture] 共享内存名称不能为空" << std::endl;
        return -1;
    }

    const size_t mapping_size =
        sizeof(smile2unlock::SharedFrameHeader) +
        smile2unlock::SharedFrameHeader::MAX_IMAGE_BYTES +
        smile2unlock::SharedFrameHeader::MAX_FEATURE_BYTES;
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, map_name.c_str());
    if (mapping == nullptr) {
        std::cerr << "[Capture] 打开共享内存失败, GetLastError=" << GetLastError() << std::endl;
        return -1;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapping_size);
    if (view == nullptr) {
        CloseHandle(mapping);
        std::cerr << "[Capture] 映射共享内存失败, GetLastError=" << GetLastError() << std::endl;
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

    if (extract_feature) {
        try {
            seetaface recognizer;
            auto feature = recognizer.img2features(image);
            if (!feature) {
                header->status_code = static_cast<int32_t>(smile2unlock::SharedFrameStatus::FEATURE_EXTRACTION_FAILED);
            } else {
                const int feature_size = recognizer.feature_size();
                const auto* feature_bytes = reinterpret_cast<const unsigned char*>(feature.get());
                const std::string feature_hex = EncodeBytesToHex(feature_bytes, static_cast<size_t>(feature_size) * sizeof(float));
                if (feature_hex.size() > smile2unlock::SharedFrameHeader::MAX_FEATURE_BYTES) {
                    header->status_code = static_cast<int32_t>(smile2unlock::SharedFrameStatus::FEATURE_TOO_LARGE);
                } else {
                    header->feature_bytes = static_cast<uint32_t>(feature_hex.size());
                    memcpy(bytes + image_bytes, feature_hex.data(), feature_hex.size());
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Capture] 特征提取失败: " << e.what() << std::endl;
            header->status_code = static_cast<int32_t>(smile2unlock::SharedFrameStatus::FEATURE_EXTRACTION_FAILED);
        }
    }

    delete[] image.data;
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return 0;
}

int streamPreviewToSharedMemory(int camera_index, const std::string& map_name) {
    if (map_name.empty()) {
        std::cerr << "[Preview] 共享内存名称不能为空" << std::endl;
        return -1;
    }

    const size_t mapping_size =
        sizeof(smile2unlock::SharedFrameHeader) +
        smile2unlock::SharedFrameHeader::MAX_IMAGE_BYTES +
        smile2unlock::SharedFrameHeader::MAX_FEATURE_BYTES;
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, map_name.c_str());
    if (mapping == nullptr) {
        std::cerr << "[Preview] 打开共享内存失败, GetLastError=" << GetLastError() << std::endl;
        return -1;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapping_size);
    if (view == nullptr) {
        CloseHandle(mapping);
        std::cerr << "[Preview] 映射共享内存失败, GetLastError=" << GetLastError() << std::endl;
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

    auto* bytes = reinterpret_cast<unsigned char*>(header + 1);
    while (header->stop_requested == 0) {
        SeetaImageData image{};
        if (!cam.CaptureFrame(image) || image.data == nullptr || image.width <= 0 || image.height <= 0) {
            header->status_code = static_cast<int32_t>(smile2unlock::SharedFrameStatus::CAPTURE_FAILED);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        const uint32_t image_bytes = static_cast<uint32_t>(image.width * image.height * image.channels);
        if (image_bytes == 0 || image_bytes > smile2unlock::SharedFrameHeader::MAX_IMAGE_BYTES) {
            header->status_code = static_cast<int32_t>(smile2unlock::SharedFrameStatus::INVALID_FRAME);
            delete[] image.data;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        memcpy(bytes, image.data, image_bytes);
        header->width = image.width;
        header->height = image.height;
        header->channels = image.channels;
        header->image_bytes = image_bytes;
        header->feature_bytes = 0;
        header->status_code = static_cast<int32_t>(smile2unlock::SharedFrameStatus::READY);
        ++header->frame_sequence;

        delete[] image.data;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return 0;
}

int compareFeatures(const std::string& feature_a_hex, const std::string& feature_b_hex) {
    if (feature_a_hex.empty() || feature_b_hex.empty()) {
        std::cerr << "[Compare] 特征字符串不能为空" << std::endl;
        return -1;
    }

    seetaface recognizer;
    const int feature_size = recognizer.feature_size();
    const size_t feature_bytes = static_cast<size_t>(feature_size) * sizeof(float);

    auto bytes_a = DecodeHexToBytes(feature_a_hex);
    auto bytes_b = DecodeHexToBytes(feature_b_hex);
    if (bytes_a.size() != feature_bytes || bytes_b.size() != feature_bytes) {
        std::cerr << "[Compare] 特征尺寸不匹配" << std::endl;
        return -1;
    }

    std::shared_ptr<float> feature_a(new float[feature_size], [](float* p) { delete[] p; });
    std::shared_ptr<float> feature_b(new float[feature_size], [](float* p) { delete[] p; });
    memcpy(feature_a.get(), bytes_a.data(), feature_bytes);
    memcpy(feature_b.get(), bytes_b.data(), feature_bytes);

    const float similarity = recognizer.feat_compare(feature_a, feature_b);
    std::cout << similarity << std::endl;
    return 0;
}

// 人脸识别函数
int recognizeFace(int camera_index, bool liveness_detection,
                  float liveness_threshold, bool debug, const std::string& result_map) {
    
    std::cout << "[Recognize] 启动人脸识别模式" << std::endl;
    
    HANDLE result_mapping = nullptr;
    void* result_view = nullptr;
    smile2unlock::SharedFrameHeader* result_header = nullptr;
    unsigned char* result_bytes = nullptr;
    if (!result_map.empty()) {
        const size_t mapping_size =
            sizeof(smile2unlock::SharedFrameHeader) +
            smile2unlock::SharedFrameHeader::MAX_IMAGE_BYTES +
            smile2unlock::SharedFrameHeader::MAX_FEATURE_BYTES;
        result_mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, result_map.c_str());
        if (result_mapping != nullptr) {
            result_view = MapViewOfFile(result_mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapping_size);
            if (result_view != nullptr) {
                result_header = static_cast<smile2unlock::SharedFrameHeader*>(result_view);
                *result_header = smile2unlock::SharedFrameHeader{};
                result_bytes = reinterpret_cast<unsigned char*>(result_header + 1);
            } else {
                CloseHandle(result_mapping);
                result_mapping = nullptr;
            }
        }
    }

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

                    if (result_header != nullptr && result_bytes != nullptr) {
                        const int feature_size = recognizer.feature_size();
                        const auto* feature_bytes = reinterpret_cast<const unsigned char*>(current_features.get());
                        const std::string feature_hex = EncodeBytesToHex(
                            feature_bytes,
                            static_cast<size_t>(feature_size) * sizeof(float));
                        if (feature_hex.size() <= smile2unlock::SharedFrameHeader::MAX_FEATURE_BYTES) {
                            result_header->feature_bytes = static_cast<uint32_t>(feature_hex.size());
                            result_header->status_code = static_cast<int32_t>(smile2unlock::SharedFrameStatus::READY);
                            memcpy(result_bytes, feature_hex.data(), feature_hex.size());
                        } else {
                            result_header->status_code = static_cast<int32_t>(smile2unlock::SharedFrameStatus::FEATURE_TOO_LARGE);
                        }
                    }

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

    if (result_view != nullptr) {
        UnmapViewOfFile(result_view);
    }
    if (result_mapping != nullptr) {
        CloseHandle(result_mapping);
    }
    
    std::cout << "人脸识别结束。" << std::endl;
    return 0;
}

int mainOptimized(int argc, char* argv[]) {
    cxxopts::Options options("FaceRecognizer",
        "人脸识别工具");
    
    options.add_options()
        ("h,help", "显示帮助信息")
        ("m,mode", "操作模式: recognize, capture-image, preview-stream, compare-features, test",
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
        ("result-map", "Shared memory mapping name for recognition result feature",
         cxxopts::value<std::string>()->default_value(""))
        ("feature-a", "First feature hex string for compare-features mode",
         cxxopts::value<std::string>()->default_value(""))
        ("feature-b", "Second feature hex string for compare-features mode",
         cxxopts::value<std::string>()->default_value(""))
        ("extract-feature", "Extract feature and append it into shared memory payload",
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
    std::string result_map = result["result-map"].as<std::string>();
    std::string feature_a_hex = result["feature-a"].as<std::string>();
    std::string feature_b_hex = result["feature-b"].as<std::string>();
    const bool extract_feature = result["extract-feature"].as<bool>();
    g_udp_port = result["udp-port"].as<int>();
    
    // 加载配置
    ConfigManager config_manager(smile2unlock::paths::GetConfigIniPath().string());
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
    
    
    if (!g_udp_sender) {
        g_udp_sender = std::make_unique<UdpSender>("127.0.0.1", g_udp_port);
    }

    // 根据模式执行相应操作
    if (mode == "recognize") {
        std::cout << "=== 人脸识别模式 ===" << std::endl;
                
        return recognizeFace(config.camera,
                                     config.liveness, liveness_threshold,
                                     config.debug, result_map);
    }
    else if (mode == "capture-image") {
        std::cout << "=== 单帧抓拍模式 ===" << std::endl;
        return captureImageToSharedMemory(config.camera, frame_map, extract_feature);
    }
    else if (mode == "preview-stream") {
        std::cout << "=== 实时预览模式 ===" << std::endl;
        return streamPreviewToSharedMemory(config.camera, frame_map);
    }
    else if (mode == "compare-features") {
        return compareFeatures(feature_a_hex, feature_b_hex);
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
    InitializeProcessLogging();
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
