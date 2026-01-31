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
inline std::atomic<int> face_recognition_status{0};

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
    int get_status() const { return face_recognition_status.load(); }

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
