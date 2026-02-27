#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <boost/asio.hpp>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <iostream>
#include "Smile2Unlock/backend/models/udp_status_packet.h"
#include "Smile2Unlock/backend/models/recognition_status.h"
#include "Smile2Unlock/backend/models/udp_auth_request_packet.h"

namespace asio = boost::asio;
using udp = asio::ip::udp;

namespace smile2unlock {
namespace managers {

/**
 * @brief UDP 接收器 - 接收来自 FaceRecognizer 的识别结果
 * 
 * 职责：
 * - 监听 FR 发送的 UdpStatusPacket
 * - 回调通知上层应用
 */
class UdpReceiverFromFR {
public:
    using StatusCallback = std::function<void(RecognitionStatus, const std::string&)>;

    explicit UdpReceiverFromFR(uint16_t port = 51235)
        : socket_(io_context_, udp::endpoint(udp::v4(), port)),
          running_(false) {
    }

    ~UdpReceiverFromFR() {
        stop();
    }

    void set_callback(StatusCallback callback) {
        callback_ = callback;
    }

    void start() {
        if (running_) {
            return;
        }

        running_ = true;
        recv_thread_ = std::thread([this]() { receive_loop(); });
    }

    void stop() {
        if (!running_) {
            return;
        }

        running_ = false;
        boost::system::error_code ec;
        socket_.close(ec);

        if (recv_thread_.joinable()) {
            recv_thread_.join();
        }
    }

private:
    void receive_loop() {
        UdpStatusPacket packet;
        udp::endpoint sender_endpoint;

        while (running_) {
            try {
                boost::system::error_code ec;
                size_t len = socket_.receive_from(
                    asio::buffer(&packet, sizeof(packet)),
                    sender_endpoint,
                    0,
                    ec
                );

                if (ec == asio::error::operation_aborted || 
                    ec == asio::error::bad_descriptor) {
                    break;
                }

                if (ec) {
                    continue;
                }

                if (len != sizeof(UdpStatusPacket)) {
                    continue;
                }

                // 验证魔数和版本
                if (packet.magic_number != MAGIC_NUMBER || 
                    packet.version != PROTOCOL_VERSION) {
                    continue;
                }

                // 回调通知
                if (callback_) {
                    RecognitionStatus status = static_cast<RecognitionStatus>(packet.status_code);
                    std::string username(packet.username);
                    callback_(status, username);
                }
            }
            catch (const std::exception&) {
                // 忽略异常，继续接收
            }
        }
    }

    asio::io_context io_context_;
    udp::socket socket_;
    std::atomic<bool> running_;
    std::thread recv_thread_;
    StatusCallback callback_;
};

/**
 * @brief UDP 发送器 - 发送识别结果到 CredentialProvider
 * 
 * 职责：
 * - 将识别结果转发给 CP
 */
class UdpSenderToCP {
public:
    UdpSenderToCP(const std::string& host = "127.0.0.1", uint16_t port = 51234)
        : socket_(io_context_, udp::v4()),
          endpoint_(asio::ip::make_address(host), port) {
    }

    ~UdpSenderToCP() {
        boost::system::error_code ec;
        socket_.close(ec);
    }

    bool send_status(RecognitionStatus status, const std::string& username = "") {
        try {
            UdpStatusPacket packet;
            packet.magic_number = MAGIC_NUMBER;
            packet.version = PROTOCOL_VERSION;
            packet.status_code = static_cast<int32_t>(status);

            memset(packet.username, 0, sizeof(packet.username));
            if (!username.empty()) {
                strncpy_s(packet.username, sizeof(packet.username), username.c_str(), _TRUNCATE);
            }

            packet.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            socket_.send_to(asio::buffer(&packet, sizeof(packet)), endpoint_);
            return true;
        }
        catch (const std::exception&) {
            return false;
        }
    }

private:
    asio::io_context io_context_;
    udp::socket socket_;
    udp::endpoint endpoint_;
};

/**
 * @brief UDP 接收器 - 接收来自 CredentialProvider 的认证请求
 * 
 * 职责：
 * - 监听端口 51236，接收 CP 的认证请求
 * - 解析请求类型并触发回调
 * - 验证数据包格式和魔术字
 */
class UdpReceiverFromCP {
public:
    using RequestCallback = std::function<void(AuthRequestType, const std::string&, uint32_t)>;

    explicit UdpReceiverFromCP(uint16_t port = 51236)
        : socket_(io_context_, udp::endpoint(udp::v4(), port)),
          running_(false) {
        std::cout << "[UDP Receiver CP] 监听端口: " << port << std::endl;
    }

    ~UdpReceiverFromCP() {
        stop();
    }

    void set_callback(RequestCallback callback) {
        callback_ = callback;
    }

    void start() {
        if (running_) return;
        
        running_ = true;
        recv_thread_ = std::thread([this]() { receive_loop(); });
        std::cout << "[UDP Receiver CP] 接收线程已启动" << std::endl;
    }

    void stop() {
        if (!running_) return;
        
        running_ = false;
        boost::system::error_code ec;
        socket_.close(ec);
        
        if (recv_thread_.joinable()) {
            recv_thread_.join();
        }
        std::cout << "[UDP Receiver CP] 接收线程已停止" << std::endl;
    }

private:
    void receive_loop() {
        UdpAuthRequestPacket packet;
        udp::endpoint sender_endpoint;

        while (running_) {
            try {
                boost::system::error_code ec;
                size_t len = socket_.receive_from(
                    asio::buffer(&packet, sizeof(packet)),
                    sender_endpoint,
                    0,
                    ec
                );

                if (ec == asio::error::operation_aborted || 
                    ec == asio::error::bad_descriptor) {
                    break;
                }

                if (ec) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }

                // 验证数据包
                if (len != sizeof(UdpAuthRequestPacket)) {
                    std::cerr << "[UDP Receiver CP] 数据包大小错误: " << len << std::endl;
                    continue;
                }

                if (packet.magic_number != AUTH_REQUEST_MAGIC) {
                    std::cerr << "[UDP Receiver CP] 魔术字错误: 0x" 
                             << std::hex << packet.magic_number << std::endl;
                    continue;
                }

                if (packet.version != AUTH_REQUEST_VERSION) {
                    std::cerr << "[UDP Receiver CP] 协议版本不匹配: " << packet.version << std::endl;
                    continue;
                }

                // 解析请求
                AuthRequestType request_type = static_cast<AuthRequestType>(packet.request_type);
                std::string username_hint(packet.username_hint);
                uint32_t session_id = packet.session_id;

                std::cout << "[UDP Receiver CP] 收到请求: type=" << packet.request_type
                         << ", session=" << session_id
                         << ", hint=" << username_hint << std::endl;

                // 触发回调
                if (callback_) {
                    callback_(request_type, username_hint, session_id);
                }
            }
            catch (const std::exception& e) {
                if (running_) {
                    std::cerr << "[UDP Receiver CP] 异常: " << e.what() << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
    }

    asio::io_context io_context_;
    udp::socket socket_;
    std::thread recv_thread_;
    std::atomic<bool> running_;
    RequestCallback callback_;
};

} // namespace managers
} // namespace smile2unlock
