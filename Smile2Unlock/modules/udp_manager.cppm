module;

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <boost/asio.hpp>


#include "models/udp_auth_request_packet.h"
#include "models/udp_status_packet.h"
#include "models/recognition_status.h"

export module smile2unlock.udp_manager;

import std;

namespace asio = boost::asio;
using udp = asio::ip::udp;

export namespace smile2unlock::managers {

class UdpReceiverFromFR {
public:
    using StatusCallback = std::function<void(RecognitionStatus, const std::string&)>;

    explicit UdpReceiverFromFR(uint16_t port = 51235);
    ~UdpReceiverFromFR();

    void set_callback(StatusCallback callback);
    void start();
    void stop();

private:
    void receive_loop();

    asio::io_context io_context_;
    udp::socket socket_;
    std::atomic<bool> running_;
    std::thread recv_thread_;
    StatusCallback callback_;
};

class UdpSenderToCP {
public:
    UdpSenderToCP(const std::string& host = "127.0.0.1", uint16_t port = 51234);
    ~UdpSenderToCP();
    bool send_status(RecognitionStatus status, const std::string& username = "");

private:
    asio::io_context io_context_;
    udp::socket socket_;
    udp::endpoint endpoint_;
};

class UdpReceiverFromCP {
public:
    using RequestCallback = std::function<void(AuthRequestType, const std::string&, uint32_t)>;

    explicit UdpReceiverFromCP(uint16_t port = 51236);
    ~UdpReceiverFromCP();

    void set_callback(RequestCallback callback);
    void start();
    void stop();

private:
    void receive_loop();

    asio::io_context io_context_;
    udp::socket socket_;
    std::thread recv_thread_;
    std::atomic<bool> running_;
    RequestCallback callback_;
};

} // namespace smile2unlock::managers

module :private;

namespace smile2unlock::managers {

UdpReceiverFromFR::UdpReceiverFromFR(uint16_t port)
    : socket_(io_context_, udp::endpoint(udp::v4(), port)),
      running_(false) {
}

UdpReceiverFromFR::~UdpReceiverFromFR() {
    stop();
}

void UdpReceiverFromFR::set_callback(StatusCallback callback) {
    callback_ = callback;
}

void UdpReceiverFromFR::start() {
    if (running_) {
        return;
    }
    running_ = true;
    recv_thread_ = std::thread([this]() { receive_loop(); });
}

void UdpReceiverFromFR::stop() {
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

void UdpReceiverFromFR::receive_loop() {
    UdpStatusPacket packet;
    udp::endpoint sender_endpoint;

    while (running_) {
        try {
            boost::system::error_code ec;
            size_t len = socket_.receive_from(asio::buffer(&packet, sizeof(packet)), sender_endpoint, 0, ec);

            if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) {
                break;
            }
            if (ec || len != sizeof(UdpStatusPacket)) {
                continue;
            }
            if (packet.magic_number != MAGIC_NUMBER || packet.version != PROTOCOL_VERSION) {
                continue;
            }

            if (callback_) {
                callback_(static_cast<RecognitionStatus>(packet.status_code), std::string(packet.username));
            }
        } catch (const std::exception&) {
        }
    }
}

UdpSenderToCP::UdpSenderToCP(const std::string& host, uint16_t port)
    : socket_(io_context_, udp::v4()),
      endpoint_(asio::ip::make_address(host), port) {
}

UdpSenderToCP::~UdpSenderToCP() {
    boost::system::error_code ec;
    socket_.close(ec);
}

bool UdpSenderToCP::send_status(RecognitionStatus status, const std::string& username) {
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
            std::chrono::system_clock::now().time_since_epoch()).count();

        socket_.send_to(asio::buffer(&packet, sizeof(packet)), endpoint_);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

UdpReceiverFromCP::UdpReceiverFromCP(uint16_t port)
    : socket_(io_context_, udp::endpoint(udp::v4(), port)),
      running_(false) {
    std::cout << "[UDP Receiver CP] 监听端口: " << port << std::endl;
}

UdpReceiverFromCP::~UdpReceiverFromCP() {
    stop();
}

void UdpReceiverFromCP::set_callback(RequestCallback callback) {
    callback_ = callback;
}

void UdpReceiverFromCP::start() {
    if (running_) return;
    running_ = true;
    recv_thread_ = std::thread([this]() { receive_loop(); });
    std::cout << "[UDP Receiver CP] 接收线程已启动" << std::endl;
}

void UdpReceiverFromCP::stop() {
    if (!running_) return;
    running_ = false;
    boost::system::error_code ec;
    socket_.close(ec);
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
    std::cout << "[UDP Receiver CP] 接收线程已停止" << std::endl;
}

void UdpReceiverFromCP::receive_loop() {
    UdpAuthRequestPacket packet;
    udp::endpoint sender_endpoint;

    while (running_) {
        try {
            boost::system::error_code ec;
            size_t len = socket_.receive_from(asio::buffer(&packet, sizeof(packet)), sender_endpoint, 0, ec);

            if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) {
                break;
            }
            if (ec) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (len != sizeof(UdpAuthRequestPacket)) {
                std::cerr << "[UDP Receiver CP] 数据包大小错误: " << len << std::endl;
                continue;
            }
            if (packet.magic_number != AUTH_REQUEST_MAGIC) {
                std::cerr << "[UDP Receiver CP] 魔术字错误: 0x" << std::hex << packet.magic_number << std::endl;
                continue;
            }
            if (packet.version != AUTH_REQUEST_VERSION) {
                std::cerr << "[UDP Receiver CP] 协议版本不匹配: " << packet.version << std::endl;
                continue;
            }

            auto request_type = static_cast<AuthRequestType>(packet.request_type);
            std::string username_hint(packet.username_hint);
            uint32_t session_id = packet.session_id;

            std::cout << "[UDP Receiver CP] 收到请求: type=" << packet.request_type
                      << ", session=" << session_id
                      << ", hint=" << username_hint << std::endl;

            if (callback_) {
                callback_(request_type, username_hint, session_id);
            }
        } catch (const std::exception& e) {
            if (running_) {
                std::cerr << "[UDP Receiver CP] 异常: " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
}

} // namespace smile2unlock::managers
