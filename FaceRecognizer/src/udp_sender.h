#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <cstdint>
#include <thread>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")

// UDP 端口配置
#define UDP_PORT 51234
#define UDP_HOST "127.0.0.1"

// 魔术字: S2LU (Smile2Unlock)
#define MAGIC_NUMBER 0x53324C55

// 协议版本
#define PROTOCOL_VERSION 1

// 状态码定义
enum class RecognitionStatus : int32_t {
    IDLE = 0,           // 空闲/未开始
    RECOGNIZING = 1,    // 正在识别
    SUCCESS = 2,        // 识别成功
    FAILED = 3,         // 识别失败
    TIMEOUT = 4,        // 超时
    RECOGNITION_ERROR = 5,  // 错误（避免与 Windows ERROR 宏冲突）
    FACE_DETECTED = 6,  // 检测到人脸（预热模式）
    PROCESS_ENDED = 7   // 进程已结束
};

// UDP 数据包结构
#pragma pack(push, 1)
struct UdpStatusPacket {
    uint32_t magic;         // 魔术字，用于验证数据包
    uint32_t version;       // 协议版本
    int32_t status_code;    // 状态码
    char username[64];      // 识别到的用户名（可选）
    uint64_t timestamp;     // 时间戳
};
#pragma pack(pop)

// 全局状态码变量（与原IPC保持兼容）
inline std::atomic<int> udp_status_code{0};

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
