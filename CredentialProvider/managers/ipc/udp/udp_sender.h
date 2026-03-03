#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <boost/asio.hpp>
#include <string>
#include <iostream>
#include <chrono>
#include "models/udp_auth_request_packet.h"

namespace asio = boost::asio;
using udp = asio::ip::udp;

/**
 * @brief UDP 发送器 - 发送认证请求到 Smile2Unlock
 * 
 * 用途：
 * - CredentialProvider 向 Smile2Unlock 发送认证请求
 * - 端口：51236（Smile2Unlock 监听此端口）
 */
class UdpSenderToSU {
public:
    UdpSenderToSU(const std::string& host = "127.0.0.1", uint16_t port = 51236)
        : socket_(io_context_, udp::v4()),
          endpoint_(asio::ip::make_address(host), port),
          session_id_(0) {
        std::cout << "[UDP Sender] 已初始化，目标: " << host << ":" << port << std::endl;
    }

    ~UdpSenderToSU() {
        close();
    }

    /**
     * @brief 发送开始识别请求
     * @param username_hint 用户名提示（可选）
     * @return 成功返回 session_id，失败返回 0
     */
    uint32_t send_start_recognition(const std::string& username_hint = "") {
        return send_request(AuthRequestType::START_RECOGNITION, username_hint);
    }

    /**
     * @brief 发送取消识别请求
     * @return 成功返回 true
     */
    bool send_cancel_recognition() {
        return send_request(AuthRequestType::CANCEL_RECOGNITION) != 0;
    }

    /**
     * @brief 发送查询状态请求
     * @return 成功返回 true
     */
    bool send_query_status() {
        return send_request(AuthRequestType::QUERY_STATUS) != 0;
    }

private:
    uint32_t send_request(AuthRequestType type, const std::string& username_hint = "") {
        try {
            UdpAuthRequestPacket packet{};
            packet.magic_number = AUTH_REQUEST_MAGIC;
            packet.version = AUTH_REQUEST_VERSION;
            packet.request_type = static_cast<int32_t>(type);
            packet.session_id = ++session_id_;
            packet.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            // 复制用户名提示
            if (!username_hint.empty()) {
                strncpy_s(packet.username_hint, sizeof(packet.username_hint),
                         username_hint.c_str(), _TRUNCATE);
            }

            // 发送数据包
            size_t sent = socket_.send_to(
                asio::buffer(&packet, sizeof(packet)),
                endpoint_
            );

            if (sent == sizeof(packet)) {
                std::cout << "[UDP Sender] 发送请求成功: type=" << static_cast<int>(type)
                         << ", session_id=" << packet.session_id << std::endl;
                return packet.session_id;
            } else {
                std::cerr << "[UDP Sender] 发送不完整: " << sent << "/" << sizeof(packet) << std::endl;
                return 0;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "[UDP Sender] 发送失败: " << e.what() << std::endl;
            return 0;
        }
    }

    void close() {
        boost::system::error_code ec;
        socket_.close(ec);
    }

    asio::io_context io_context_;
    udp::socket socket_;
    udp::endpoint endpoint_;
    uint32_t session_id_;
};
