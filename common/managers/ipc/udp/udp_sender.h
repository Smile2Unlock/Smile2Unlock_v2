#pragma once

// Windows 头文件必须在 Boost.Asio 之前包含
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// 现在包含 Boost.Asio
#include <boost/asio.hpp>
#include <string>
#include <iostream>
#include <atomic>
#include <functional>
#include "models/recognition_status.h"
#include "models/udp_status_packet.h"
#include "udp_manager.h"  // 使用统一的端口配置

namespace asio = boost::asio;
using udp = asio::ip::udp;

/**
 * @brief UDP 发送器 - 用于发送人脸识别状态到凭证提供程序
 * 
 * 职责：
 * - 发送识别状态（IDLE, RECOGNIZING, SUCCESS, FAILED 等）
 * - 携带用户名等元数据
 */
class UdpSender {
public:
    UdpSender(const std::string& host = "127.0.0.1", 
              uint16_t port = smile2unlock::udp::UdpPorts::kCpStatusPort)
        : socket_(io_context_, udp::v4()),
          endpoint_(asio::ip::make_address(host), port) {
        std::cout << "[UDP Sender] 已初始化，目标: " << host << ":" << port << std::endl;
    }

    ~UdpSender() {
        std::cout << "[UDP Sender] 已关闭" << std::endl;
    }

    bool send_status(RecognitionStatus status,
                     const std::string& username = "",
                     uint32_t session_id = 0,
                     const std::string& feature = "") {
        try {
            UdpStatusPacket packet{};
            packet.magic_number = MAGIC_NUMBER;
            packet.version = PROTOCOL_VERSION;
            packet.status_code = static_cast<int32_t>(status);
            packet.session_id = session_id;

            if (!username.empty()) {
                strncpy_s(packet.username, sizeof(packet.username), username.c_str(), _TRUNCATE);
            }
            if (!feature.empty()) {
                if (feature.size() >= sizeof(packet.feature)) {
                    std::cerr << "[UDP Sender] 特征数据过大，无法发送: " << feature.size() << std::endl;
                    return false;
                }
                packet.feature_bytes = static_cast<uint32_t>(feature.size());
                memcpy(packet.feature, feature.data(), feature.size());
            }

            packet.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            socket_.send_to(asio::buffer(&packet, sizeof(packet)), endpoint_);

            std::cout << "[UDP Sender] 已发送状态: " << static_cast<int>(status)
                      << ", 用户: " << (username.empty() ? "(无)" : username)
                      << ", 会话: " << session_id
                      << ", 特征字节: " << packet.feature_bytes << std::endl;

            return true;
        }
        catch (const std::exception& e) {
            std::cerr << "[UDP Sender] 发送失败: " << e.what() << std::endl;
            return false;
        }
    }

private:
    asio::io_context io_context_;
    udp::socket socket_;
    udp::endpoint endpoint_;
};
