#pragma once

#include "Smile2Unlock/backend/models/types.h"
#include <string>
#include <memory>
#include <windows.h>

namespace smile2unlock {
namespace managers {

/**
 * @brief 人脸识别管理器 - 通过 IPC 调用 FaceRecognizer.exe
 * 
 * 架构：
 * Smile2Unlock (GUI) -> FaceRecognition (Manager) -> FaceRecognizer.exe (独立进程)
 *                                                      ↓
 *                                                  SeetaFace6
 */
class FaceRecognition {
public:
    FaceRecognition();
    ~FaceRecognition();

    bool Initialize(std::string& error_message);
    bool IsInitialized() const { return initialized_; }
    
    bool StartRecognition();
    void StopRecognition();
    bool IsRunning() const { return is_running_; }
    
    RecognitionResult GetLastResult() const;
    
    // 从图像文件提取人脸特征（调用 FaceRecognizer.exe）
    std::string ExtractFaceFeature(const std::string& image_path);
    
    // 比较两个人脸特征的相似度（调用 FaceRecognizer.exe）
    bool CompareFaces(const std::string& feature1, const std::string& feature2, float& similarity);

private:
    bool initialized_;
    bool is_running_;
    RecognitionResult last_result_;
    
    // FaceRecognizer 进程句柄
    HANDLE fr_process_;
    
    // TODO: UDP 通信模块
    // std::unique_ptr<UdpClient> udp_client_;
};

} // namespace managers
} // namespace smile2unlock
