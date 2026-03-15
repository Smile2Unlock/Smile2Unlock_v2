#pragma once

#include "Smile2Unlock/backend/models/types.h"
#include "Smile2Unlock/backend/managers/ipc/udp_manager.h"
#include <string>
#include <memory>
#include <windows.h>
#include <thread>
#include <vector>

namespace smile2unlock {
namespace managers {

/**
 * @brief 人脸识别管理器 - 中间人模式
 * 
 * 架构：
 * CredentialProvider → [UDP 51236] → Smile2Unlock → [启动并监听] → FaceRecognizer
 *                                       ↓ 转发结果
 *                      CredentialProvider ← [UDP 51234] ← Smile2Unlock
 * 
 * 完整通信流程：
 * 1. CP → SU: 发送认证请求到端口 51236
 * 2. SU: 启动 FR 进程
 * 3. FR → SU: 发送识别结果到端口 51235
 * 4. SU → CP: 转发结果到端口 51234
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
    
    // 从图像文件提取人脸特征（TODO: 需要 FR 支持命令模式）
    std::string ExtractFaceFeature(const std::string& image_path);
    
    // 比较两个人脸特征的相似度（TODO: 需要 FR 支持命令模式）
    bool CompareFaces(const std::string& feature1, const std::string& feature2, float& similarity);

private:
    bool initialized_;
    bool is_running_;
    RecognitionResult last_result_;
    
    // FaceRecognizer 进程句柄
    HANDLE fr_process_;
    PROCESS_INFORMATION fr_process_info_;
    HANDLE fr_stdout_read_;
    HANDLE fr_stderr_read_;
    std::thread log_thread_;
    bool log_thread_running_;
    
    void ReadSubProcessLogs();
    
    // UDP 通信模块
    std::unique_ptr<UdpReceiverFromCP> udp_receiver_cp_;   // 监听 CP 请求 (端口 51236)
    std::unique_ptr<UdpReceiverFromFR> udp_receiver_fr_;   // 监听 FR 结果 (端口 51235)
    std::unique_ptr<UdpSenderToCP> udp_sender_;            // 发送给 CP (端口 51234)
    
    uint32_t current_session_id_;  // 当前会话 ID
    
    // 辅助方法
    bool start_fr_process();
    void stop_fr_process();
    void on_cp_request_received(AuthRequestType type, const std::string& username_hint, uint32_t session_id);
    void on_fr_status_received(RecognitionStatus status, const std::string& username);
};

} // namespace managers
} // namespace smile2unlock
