/**
 * @file FaceAuthClient.h
 * @brief IPC客户端封装，用于CredentialProvider与Smile2Unlock Server通信
 * @date 2026-01-18
 */

#pragma once
#include "../Smile2Unlock/ipc/IPCReceiver.h"
#include <memory>
#include <string>

/**
 * @brief 人脸认证客户端
 * 
 * 封装IPC通信细节，提供简洁的认证接口供CredentialProvider使用
 */
class FaceAuthClient {
public:
    FaceAuthClient();
    ~FaceAuthClient();

    /**
     * @brief 连接到Smile2Unlock服务
     * @return true 连接成功, false 连接失败
     */
    bool Connect();

    /**
     * @brief 断开连接
     */
    void Disconnect();

    /**
     * @brief 启动人脸认证
     * @param userId 用户ID
     * @param timeoutMs 超时时间(毫秒)
     * @return true 认证成功, false 认证失败
     */
    bool Authenticate(const std::string& userId, DWORD timeoutMs = 30000);

    /**
     * @brief 获取当前认证状态
     * @return 状态码: 0-空闲, 1-识别中, 2-成功, 3-失败, 4-活体检测失败
     */
    int GetStatus();

    /**
     * @brief 获取最后的错误消息
     * @return 错误消息字符串
     */
    std::string GetLastError() const;

    /**
     * @brief 检查服务是否可用
     * @return true 服务可用, false 服务不可用
     */
    bool IsServiceAvailable();

private:
    std::unique_ptr<IPCReceiver> receiver_;
    std::string lastError_;
    bool connected_;
};
