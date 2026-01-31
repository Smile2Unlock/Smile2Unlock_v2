#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <cstdint>
#include <thread>
#include <atomic>

#include "models/recognition_status.h"
#include "models/udp_status_packet.h"

#pragma comment(lib, "ws2_32.lib")

// UDP 端口配置
#define UDP_PORT 51234
#define UDP_HOST "127.0.0.1"

// 全局状态码变量（与原IPC保持兼容）
inline std::atomic<RecognitionStatus> udp_status_code{RecognitionStatus::IDLE};

class udp_sender {
public:
    udp_sender();
    ~udp_sender();

    // 发送状态码
    bool send_status(RecognitionStatus status, const std::string& username = "");

    // 启动自动发送线程（轮询 udp_status_code）
    void start_auto_send();

    // 停止自动发送线程
    void stop_auto_send();

private:
    SOCKET sock;
    sockaddr_in server_addr;
    bool initialized;

    std::thread send_thread;
    std::atomic<bool> auto_send_running{false};

    void auto_send_loop();
};
