module;

#include "../protocol/messages.h"

export module su.facerecognizer.socket_sender;

import std;

export namespace su::facerecognizer {

/**
 * @brief Socket 发送器
 * 
 * 用于向主应用程序发送消息（占位实现）。
 */
class SocketSender {
public:
    /**
     * @brief 构造函数
     * @param endpoint 连接端点地址
     */
    explicit SocketSender(std::string endpoint) : endpoint_(std::move(endpoint)) {}

    /**
     * @brief 连接到端点
     * 
     * 当前为占位实现，仅检查端点是否为空。
     * 
     * @return bool 是否连接成功
     */
    bool connect() {
        connected_ = !endpoint_.empty();
        return connected_;
    }

    /**
     * @brief 发送消息
     * 
     * 当前为占位实现，仅保存消息到内部缓冲区。
     * 
     * @param message 要发送的消息
     * @return bool 是否发送成功
     */
    bool send(const protocol::OutboundMessage& message) {
        if (!connected_) {
            return false;
        }
        last_message_ = message;
        return true;
    }

    /**
     * @brief 获取最后发送的消息
     * @return const protocol::OutboundMessage& 最后发送的消息
     */
    const protocol::OutboundMessage& last_message() const {
        return last_message_;
    }

private:
    std::string endpoint_;
    bool connected_ = false;
    protocol::OutboundMessage last_message_{};
};

}  // namespace su::facerecognizer
