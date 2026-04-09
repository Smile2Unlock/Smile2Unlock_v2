#pragma once

/**
 * @file udp_manager.h
 * @brief 统一 UDP 通讯管理器
 *
 * 整合所有 UDP 通讯组件:
 * - UdpSender: 发送状态到 CP (端口 51234)
 * - UdpReceiver: 接收状态 (端口 51234)
 * - AuthRequestSender: 发送认证请求到 SU (端口 51236)
 * - AuthRequestReceiver: 接收认证请求 (端口 51236)
 * - PasswordSender: 发送密码响应到 CP (端口 51237)
 * - PasswordReceiver: 接收密码响应 (端口 51237)
 * - StatusSender: 发送状态到 SU (端口 51235)
 * - StatusReceiver: 接收状态从 FR (端口 51235)
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>

#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "models/recognition_status.h"
#include "models/udp_status_packet.h"
#include "models/udp_auth_request_packet.h"
#include "models/udp_password_packet.h"

namespace asio = boost::asio;
using udp = asio::ip::udp;

namespace smile2unlock::udp {

// ============================================================================
// 端口配置
// ============================================================================
struct UdpPorts {
    static constexpr uint16_t kCpStatusPort = 51234;      // CP 接收状态 / FR 发送状态
    static constexpr uint16_t kFrStatusPort = 51235;      // SU 接收 FR 状态
    static constexpr uint16_t kAuthRequestPort = 51236;   // SU 接收 CP 认证请求
    static constexpr uint16_t kPasswordPort = 51237;      // CP 接收密码响应
};

// ============================================================================
// 状态发送器 (FR -> CP / SU -> CP)
// ============================================================================
class StatusSender {
public:
    StatusSender(const std::string& host = "127.0.0.1", uint16_t port = UdpPorts::kCpStatusPort)
        : socket_(io_context_, ::udp::v4()),
          endpoint_(asio::ip::make_address(host), port),
          initialized_(false) {
        boost::system::error_code ec;
        if (socket_.is_open()) {
            initialized_ = true;
        }
    }

    ~StatusSender() {
        boost::system::error_code ec;
        socket_.close(ec);
    }

    bool send_status(RecognitionStatus status,
                     const std::string& username = "",
                     uint32_t session_id = 0,
                     const std::string& feature = "") {
        if (!initialized_) return false;

        try {
            UdpStatusPacket packet{};
            packet.magic_number = MAGIC_NUMBER;
            packet.version = PROTOCOL_VERSION;
            packet.status_code = static_cast<int32_t>(status);
            packet.session_id = session_id;
            packet.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            if (!username.empty()) {
                strncpy_s(packet.username, sizeof(packet.username), username.c_str(), _TRUNCATE);
            }
            if (!feature.empty()) {
                if (feature.size() >= sizeof(packet.feature)) {
                    return false;
                }
                packet.feature_bytes = static_cast<uint32_t>(feature.size());
                memcpy(packet.feature, feature.data(), feature.size());
            }

            socket_.send_to(asio::buffer(&packet, sizeof(packet)), endpoint_);

            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    bool is_initialized() const { return initialized_; }

private:
    asio::io_context io_context_;
    ::udp::socket socket_;
    ::udp::endpoint endpoint_;
    bool initialized_;
};

// ============================================================================
// 状态接收器 (CP/SU 接收)
// ============================================================================
class StatusReceiver {
public:
    using StatusCallback = std::function<void(RecognitionStatus, const std::string&, uint32_t, const std::string&)>;

    explicit StatusReceiver(uint16_t port = UdpPorts::kCpStatusPort)
        : socket_(io_context_, ::udp::endpoint(::udp::v4(), port)),
          running_(false) {}

    ~StatusReceiver() { stop(); }

    void set_callback(StatusCallback callback) { callback_ = std::move(callback); }

    void start() {
        if (running_) return;
        running_ = true;
        recv_thread_ = std::thread([this]() { receive_loop(); });
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        boost::system::error_code ec;
        socket_.close(ec);
        if (recv_thread_.joinable()) {
            recv_thread_.join();
        }
    }

    bool is_running() const { return running_; }

private:
    void receive_loop() {
        UdpStatusPacket packet{};
        ::udp::endpoint sender_endpoint;

        while (running_) {
            try {
                boost::system::error_code ec;
                size_t len = socket_.receive_from(
                    asio::buffer(&packet, sizeof(packet)), sender_endpoint, 0, ec);

                if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) {
                    break;
                }
                if (ec || len != sizeof(UdpStatusPacket)) continue;
                if (packet.magic_number != MAGIC_NUMBER || packet.version != PROTOCOL_VERSION) continue;

                if (callback_) {
                    std::string feature;
                    if (packet.feature_bytes > 0 && packet.feature_bytes <= sizeof(packet.feature)) {
                        feature.assign(packet.feature, packet.feature + packet.feature_bytes);
                    }
                    callback_(static_cast<RecognitionStatus>(packet.status_code),
                              std::string(packet.username),
                              packet.session_id,
                              feature);
                }
            } catch (const std::exception&) {
                if (running_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        }
    }

    asio::io_context io_context_;
    ::udp::socket socket_;
    std::atomic<bool> running_;
    std::thread recv_thread_;
    StatusCallback callback_;
};

// ============================================================================
// 认证请求发送器 (CP -> SU)
// ============================================================================
class AuthRequestSender {
public:
    AuthRequestSender(const std::string& host = "127.0.0.1", uint16_t port = UdpPorts::kAuthRequestPort)
        : socket_(io_context_, ::udp::v4()),
          endpoint_(asio::ip::make_address(host), port),
          initialized_(false) {
        boost::system::error_code ec;
        if (socket_.is_open()) {
            initialized_ = true;
        }
    }

    ~AuthRequestSender() {
        boost::system::error_code ec;
        socket_.close(ec);
    }

    bool send_request(AuthRequestType request_type,
                      const std::string& username_hint = "",
                      uint32_t session_id = 0) {
        if (!initialized_) return false;

        try {
            UdpAuthRequestPacket packet{};
            packet.magic_number = AUTH_REQUEST_MAGIC;
            packet.version = AUTH_REQUEST_VERSION;
            packet.request_type = static_cast<int32_t>(request_type);
            packet.session_id = session_id;
            packet.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            if (!username_hint.empty()) {
                strncpy_s(packet.username_hint, sizeof(packet.username_hint),
                          username_hint.c_str(), _TRUNCATE);
            }

            socket_.send_to(asio::buffer(&packet, sizeof(packet)), endpoint_);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    bool is_initialized() const { return initialized_; }

private:
    asio::io_context io_context_;
    ::udp::socket socket_;
    ::udp::endpoint endpoint_;
    bool initialized_;
};

// ============================================================================
// 认证请求接收器 (SU 接收)
// ============================================================================
class AuthRequestReceiver {
public:
    using RequestCallback = std::function<void(AuthRequestType, const std::string&, uint32_t)>;

    explicit AuthRequestReceiver(uint16_t port = UdpPorts::kAuthRequestPort)
        : socket_(io_context_, ::udp::endpoint(::udp::v4(), port)),
          running_(false) {}

    ~AuthRequestReceiver() { stop(); }

    void set_callback(RequestCallback callback) { callback_ = std::move(callback); }

    void start() {
        if (running_) return;
        running_ = true;
        recv_thread_ = std::thread([this]() { receive_loop(); });
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        boost::system::error_code ec;
        socket_.close(ec);
        if (recv_thread_.joinable()) {
            recv_thread_.join();
        }
    }

    bool is_running() const { return running_; }

private:
    void receive_loop() {
        UdpAuthRequestPacket packet{};
        ::udp::endpoint sender_endpoint;

        while (running_) {
            try {
                boost::system::error_code ec;
                size_t len = socket_.receive_from(
                    asio::buffer(&packet, sizeof(packet)), sender_endpoint, 0, ec);

                if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) {
                    break;
                }
                if (ec || len != sizeof(UdpAuthRequestPacket)) continue;
                if (packet.magic_number != AUTH_REQUEST_MAGIC || packet.version != AUTH_REQUEST_VERSION) continue;

                if (callback_) {
                    callback_(static_cast<AuthRequestType>(packet.request_type),
                              std::string(packet.username_hint),
                              packet.session_id);
                }
            } catch (const std::exception&) {
                if (running_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        }
    }

    asio::io_context io_context_;
    ::udp::socket socket_;
    std::atomic<bool> running_;
    std::thread recv_thread_;
    RequestCallback callback_;
};

// ============================================================================
// 密码响应发送器 (SU -> CP)
// ============================================================================
class PasswordSender {
public:
    PasswordSender(const std::string& host = "127.0.0.1", uint16_t port = UdpPorts::kPasswordPort)
        : socket_(io_context_, ::udp::v4()),
          endpoint_(asio::ip::make_address(host), port),
          initialized_(false) {
        boost::system::error_code ec;
        if (socket_.is_open()) {
            initialized_ = true;
        }
    }

    ~PasswordSender() {
        boost::system::error_code ec;
        socket_.close(ec);
    }

    bool send_password(uint32_t session_id, const std::string& username,
                       const std::string& password, bool success) {
        if (!initialized_) return false;

        try {
            UdpPasswordResponsePacket packet{};
            packet.magic_number = PASSWORD_RESPONSE_MAGIC;
            packet.version = PASSWORD_RESPONSE_VERSION;
            packet.session_id = session_id;
            packet.result_code = success ? 0 : 1;
            packet.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            strncpy_s(packet.username, sizeof(packet.username), username.c_str(), _TRUNCATE);

            if (success && !password.empty()) {
                DATA_BLOB input_blob{};
                input_blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(password.data()));
                input_blob.cbData = static_cast<DWORD>(password.size());

                DATA_BLOB output_blob{};
                if (!CryptProtectData(&input_blob, nullptr, nullptr, nullptr, nullptr, 0, &output_blob)) {
                    return false;
                }

                const DWORD copy_size = std::min<DWORD>(output_blob.cbData, sizeof(packet.protected_password));
                if (copy_size != output_blob.cbData) {
                    SecureZeroMemory(output_blob.pbData, output_blob.cbData);
                    LocalFree(output_blob.pbData);
                    return false;
                }

                packet.protected_password_size = copy_size;
                memcpy(packet.protected_password, output_blob.pbData, copy_size);
                SecureZeroMemory(output_blob.pbData, output_blob.cbData);
                LocalFree(output_blob.pbData);
            }

            socket_.send_to(asio::buffer(&packet, sizeof(packet)), endpoint_);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    bool is_initialized() const { return initialized_; }

private:
    asio::io_context io_context_;
    ::udp::socket socket_;
    ::udp::endpoint endpoint_;
    bool initialized_;
};

// ============================================================================
// 密码响应接收器 (CP 接收)
// ============================================================================
class PasswordReceiver {
public:
    using PasswordCallback = std::function<void(uint32_t, const std::string&, const std::string&, bool)>;

    explicit PasswordReceiver(uint16_t port = UdpPorts::kPasswordPort)
        : socket_(io_context_, ::udp::endpoint(::udp::v4(), port)),
          running_(false) {}

    ~PasswordReceiver() { stop(); }

    void set_callback(PasswordCallback callback) { callback_ = std::move(callback); }

    void start() {
        if (running_) return;
        running_ = true;
        recv_thread_ = std::thread([this]() { receive_loop(); });
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        boost::system::error_code ec;
        socket_.close(ec);
        if (recv_thread_.joinable()) {
            recv_thread_.join();
        }
    }

    bool is_running() const { return running_; }

private:
    void receive_loop() {
        UdpPasswordResponsePacket packet{};
        ::udp::endpoint sender_endpoint;

        while (running_) {
            try {
                boost::system::error_code ec;
                size_t len = socket_.receive_from(
                    asio::buffer(&packet, sizeof(packet)), sender_endpoint, 0, ec);

                if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) {
                    break;
                }
                if (ec || len != sizeof(UdpPasswordResponsePacket)) continue;
                if (packet.magic_number != PASSWORD_RESPONSE_MAGIC || packet.version != PASSWORD_RESPONSE_VERSION) continue;

                std::string password;
                bool success = (packet.result_code == 0);

                if (success && packet.protected_password_size > 0) {
                    DATA_BLOB input_blob{};
                    input_blob.pbData = const_cast<BYTE*>(packet.protected_password);
                    input_blob.cbData = packet.protected_password_size;

                    DATA_BLOB output_blob{};
                    if (CryptUnprotectData(&input_blob, nullptr, nullptr, nullptr, nullptr, 0, &output_blob)) {
                        password.assign(reinterpret_cast<char*>(output_blob.pbData), output_blob.cbData);
                        SecureZeroMemory(output_blob.pbData, output_blob.cbData);
                        LocalFree(output_blob.pbData);
                    }
                }

                if (callback_) {
                    callback_(packet.session_id, std::string(packet.username), password, success);
                }

                // 清理敏感数据
                SecureZeroMemory(packet.protected_password, sizeof(packet.protected_password));
            } catch (const std::exception&) {
                if (running_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        }
    }

    asio::io_context io_context_;
    ::udp::socket socket_;
    std::atomic<bool> running_;
    std::thread recv_thread_;
    PasswordCallback callback_;
};

} // namespace smile2unlock::udp
