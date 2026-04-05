#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <boost/asio.hpp>
#include <chrono>
#include <cstring>
#include <string>

#include "models/udp_auth_request_packet.h"
#include "udp_manager.h"  // 使用统一的端口配置

namespace asio = boost::asio;
using udp = asio::ip::udp;

class AuthRequestSender {
public:
    AuthRequestSender(const std::string& host = "127.0.0.1", 
                      uint16_t port = smile2unlock::udp::UdpPorts::kAuthRequestPort)
        : socket_(io_context_, udp::v4()),
          endpoint_(asio::ip::make_address(host), port) {
    }

    bool send_request(AuthRequestType request_type,
                      const std::string& username_hint = "",
                      uint32_t session_id = 0) {
        try {
            UdpAuthRequestPacket packet{};
            packet.magic_number = AUTH_REQUEST_MAGIC;
            packet.version = AUTH_REQUEST_VERSION;
            packet.request_type = static_cast<int32_t>(request_type);
            packet.session_id = session_id;
            packet.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            if (!username_hint.empty()) {
                strncpy_s(packet.username_hint, sizeof(packet.username_hint), username_hint.c_str(), _TRUNCATE);
            }

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
