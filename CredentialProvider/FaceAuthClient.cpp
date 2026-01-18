/**
 * @file FaceAuthClient.cpp
 * @brief IPC客户端实现
 */

#include "FaceAuthClient.h"
#include <sstream>

FaceAuthClient::FaceAuthClient() 
    : receiver_(std::make_unique<IPCReceiver>())
    , connected_(false) {
}

FaceAuthClient::~FaceAuthClient() {
    Disconnect();
}

bool FaceAuthClient::Connect() {
    if (connected_) {
        return true;
    }

    if (!receiver_->Initialize()) {
        lastError_ = "Failed to initialize IPC receiver. Is Smile2Unlock Server running?";
        return false;
    }

    connected_ = true;
    lastError_.clear();
    return true;
}

void FaceAuthClient::Disconnect() {
    if (connected_) {
        receiver_->Cleanup();
        connected_ = false;
    }
}

bool FaceAuthClient::Authenticate(const std::string& userId, DWORD timeoutMs) {
    if (!connected_ && !Connect()) {
        return false;
    }

    // 等待认证完成
    DWORD startTime = GetTickCount();
    DWORD elapsed = 0;

    while (elapsed < timeoutMs) {
        int status = receiver_->GetStatus(500);  // 每次等待500ms

        switch (status) {
            case 2:  // 认证成功
                lastError_.clear();
                return true;

            case 3:  // 认证失败
                lastError_ = "Face recognition failed";
                return false;

            case 4:  // 活体检测失败
                lastError_ = "Liveness detection failed";
                return false;

            case -1:  // 初始化失败
                lastError_ = "IPC connection lost";
                connected_ = false;
                return false;

            case -2:  // 超时
                // 继续等待
                break;

            case 0:   // 空闲
            case 1:   // 识别中
                // 继续等待
                break;

            default:
                lastError_ = "Unknown status code: " + std::to_string(status);
                return false;
        }

        elapsed = GetTickCount() - startTime;
    }

    lastError_ = "Authentication timeout";
    return false;
}

int FaceAuthClient::GetStatus() {
    if (!connected_) {
        return -1;
    }

    return receiver_->GetStatus(100);
}

std::string FaceAuthClient::GetLastError() const {
    return lastError_;
}

bool FaceAuthClient::IsServiceAvailable() {
    if (!connected_) {
        return Connect();
    }

    // 尝试获取状态来检测服务是否存活
    int status = receiver_->GetStatus(100);
    return status != -1;
}
