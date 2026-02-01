#pragma once

// Windows 头文件必须在 Boost.Asio 之前包含
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// 现在包含 Boost.Asio
#include <boost/asio.hpp>
#include <thread>
#include <atomic>
#include <functional>
#include <iostream>
#include <string>
#include "models/recognition_status.h"
#include "models/udp_status_packet.h"

namespace asio = boost::asio;
using udp = asio::ip::udp;

inline std::atomic<RecognitionStatus> face_recognition_status{RecognitionStatus::IDLE};
inline std::string recognized_username;

class UdpReceiver {
public:
    explicit UdpReceiver(uint16_t port = 51234)
        : socket_(io_context_, udp::endpoint(udp::v4(), port)),
          running_(false) {
        std::cout << "[UDP Receiver] 已初始化，监听端口: " << port << std::endl;
        start();
    }

    ~UdpReceiver() {
        stop();
        std::cout << "[UDP Receiver] 已关闭" << std::endl;
    }

    void set_callback(std::function<void(RecognitionStatus, const std::string&)> callback) {
        status_callback_ = callback;
    }

    RecognitionStatus get_status() const {
        return face_recognition_status.load();
    }

    std::string get_username() const {
        return recognized_username;
    }

private:
    void start() {
        if (running_) {
            std::cout << "[UDP Receiver] 接收线程已在运行" << std::endl;
            return;
        }

        running_ = true;
        recv_thread_ = std::thread([this]() { receive_loop(); });
        std::cout << "[UDP Receiver] 接收线程已启动" << std::endl;
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

        std::cout << "[UDP Receiver] 接收线程已停止" << std::endl;
    }

    void receive_loop() {
        UdpStatusPacket packet;
        udp::endpoint sender_endpoint;

        std::cout << "[UDP Receiver] 接收循环开始" << std::endl;

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
                    std::cerr << "[UDP Receiver] 接收错误: " << ec.message() << std::endl;
                    continue;
                }

                if (len != sizeof(UdpStatusPacket)) {
                    std::cerr << "[UDP Receiver] 数据包大小不匹配: " << len << std::endl;
                    continue;
                }

                if (packet.magic_number != MAGIC_NUMBER) {
                    std::cerr << "[UDP Receiver] 无效的魔术字: 0x" 
                              << std::hex << packet.magic_number << std::dec << std::endl;
                    continue;
                }

                if (packet.version != PROTOCOL_VERSION) {
                    std::cerr << "[UDP Receiver] 协议版本不匹配: " << packet.version << std::endl;
                    continue;
                }

                RecognitionStatus old_status = face_recognition_status.exchange(
                    static_cast<RecognitionStatus>(packet.status_code)
                );

                if (packet.username[0] != '\0') {
                    recognized_username = std::string(packet.username);
                }

                std::cout << "[UDP Receiver] 收到状态更新: " << packet.status_code
                          << " (旧状态: " << static_cast<int>(old_status) << ")"
                          << ", 用户: " << (packet.username[0] ? packet.username : "(无)") << std::endl;

                if (status_callback_) {
                    status_callback_(
                        static_cast<RecognitionStatus>(packet.status_code),
                        std::string(packet.username)
                    );
                }
            }
            catch (const std::exception& e) {
                if (running_) {
                    std::cerr << "[UDP Receiver] 异常: " << e.what() << std::endl;
                }
            }
        }

        std::cout << "[UDP Receiver] 接收循环结束" << std::endl;
    }

    asio::io_context io_context_;
    udp::socket socket_;
    std::thread recv_thread_;
    std::atomic<bool> running_;
    std::function<void(RecognitionStatus, const std::string&)> status_callback_;
};
