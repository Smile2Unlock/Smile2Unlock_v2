#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <cstdint>
#include <thread>
#include <atomic>
#include <functional>

#include "models/recognition_status.h"
#include "models/udp_status_packet.h"

#pragma comment(lib, "ws2_32.lib")

// UDP 端口配置
#define UDP_PORT 51234
#define UDP_HOST "127.0.0.1"

// 全局状态码变量（与原IPC保持兼容）
inline std::atomic<RecognitionStatus> face_recognition_status{RecognitionStatus::IDLE};

// 全局用户名变量
inline std::string recognized_username;

class udp_receiver {
public:
    udp_receiver();
    ~udp_receiver();

    // 启动接收线程
    void start();

    // 停止接收线程
    void stop();

    // 设置状态回调函数（可选）
    void set_callback(std::function<void(RecognitionStatus, const std::string&)> callback);

    // 获取最新状态
    RecognitionStatus get_status() const { return face_recognition_status.load(); }

    // 获取识别的用户名
    std::string get_username() const { return recognized_username; }

private:
    SOCKET sock;
    sockaddr_in server_addr;
    bool initialized;

    std::thread recv_thread;
    std::atomic<bool> recv_running{false};

    std::function<void(RecognitionStatus, const std::string&)> status_callback;

    void receive_loop();
};
