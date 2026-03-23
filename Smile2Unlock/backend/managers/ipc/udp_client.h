#pragma once

#define WIN32_LEAN_AND_MEAN  // 避免 winsock.h 和 winsock2.h 冲突

#include "common/models/fr_command_packet.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <boost/asio.hpp>
#include <string>
#include <memory>
#include <chrono>
#include <atomic>

namespace asio = boost::asio;
using udp = asio::ip::udp;

namespace smile2unlock {
namespace managers {

/**
 * @brief UDP 客户端 - 用于向 FaceRecognizer 发送命令并接收响应
 */
class UdpClient {
public:
    UdpClient(const std::string& host = "127.0.0.1", uint16_t port = 51235)
        : socket_(io_context_, udp::v4()),
          endpoint_(asio::ip::make_address(host), port),
          sequence_counter_(0) {
    }

    ~UdpClient() {
        close();
    }

    /**
     * @brief 发送命令并等待响应
     * @param cmd_type 命令类型
     * @param payload 负载数据
     * @param response_data 输出：响应数据
     * @param timeout_ms 超时时间（毫秒）
     * @return 成功返回 true
     */
    bool send_command(FRCommandType cmd_type, 
                      const std::string& payload,
                      std::string& response_data,
                      int timeout_ms = 5000) {
        try {
            // 构造命令包
            FRCommandPacket cmd;
            cmd.magic_number = FRCommandPacket::MAGIC_NUMBER;
            cmd.version = FRCommandPacket::PROTOCOL_VERSION;
            cmd.command_type = static_cast<int32_t>(cmd_type);
            cmd.sequence_id = ++sequence_counter_;
            
            memset(cmd.payload, 0, sizeof(cmd.payload));
            strncpy_s(cmd.payload, sizeof(cmd.payload), payload.c_str(), _TRUNCATE);
            
            cmd.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            // 发送命令
            socket_.send_to(asio::buffer(&cmd, sizeof(cmd)), endpoint_);

            // 等待响应（设置超时）
            FRResponsePacket response;
            udp::endpoint sender_endpoint;
            
            socket_.non_blocking(true);
            auto start_time = std::chrono::steady_clock::now();
            
            while (true) {
                boost::system::error_code ec;
                size_t len = socket_.receive_from(
                    asio::buffer(&response, sizeof(response)),
                    sender_endpoint,
                    0,
                    ec
                );

                if (!ec && len > 0) {
                    // 验证响应
                    if (response.magic_number == FRResponsePacket::MAGIC_NUMBER &&
                        response.sequence_id == cmd.sequence_id) {
                        
                        if (response.status_code == 0) {
                            response_data = std::string(response.result_data);
                            return true;
                        } else {
                            response_data = "Error: " + std::string(response.result_data);
                            return false;
                        }
                    }
                }

                // 检查超时
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time
                ).count();
                
                if (elapsed > timeout_ms) {
                    response_data = "Timeout waiting for response";
                    return false;
                }

                // 短暂休眠避免 CPU 占用
                Sleep(10);
            }
        }
        catch (const std::exception& e) {
            response_data = std::string("Exception: ") + e.what();
            return false;
        }
    }

    /**
     * @brief 测试与 FR 的连接
     */
    bool ping() {
        std::string response;
        return send_command(FRCommandType::PING, "", response, 1000);
    }

    void close() {
        boost::system::error_code ec;
        socket_.close(ec);
    }

private:
    asio::io_context io_context_;
    udp::socket socket_;
    udp::endpoint endpoint_;
    std::atomic<int32_t> sequence_counter_;
};

} // namespace managers
} // namespace smile2unlock
