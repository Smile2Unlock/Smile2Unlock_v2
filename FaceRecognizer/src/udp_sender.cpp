#include "udp_sender.h"
#include <chrono>
#include <cstring>

udp_sender::udp_sender() : sock(INVALID_SOCKET), initialized(false) {
    // 初始化 Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "[UDP Sender] WSAStartup 失败: " << result << std::endl;
        return;
    }

    // 创建 UDP socket
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "[UDP Sender] 创建 socket 失败: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return;
    }

    // 设置服务器地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_PORT);
    inet_pton(AF_INET, UDP_HOST, &server_addr.sin_addr);

    initialized = true;
    std::cout << "[UDP Sender] 初始化成功，目标: " << UDP_HOST << ":" << UDP_PORT << std::endl;
}

udp_sender::~udp_sender() {
    stop_auto_send();

    if (sock != INVALID_SOCKET) {
        closesocket(sock);
    }
    WSACleanup();

    std::cout << "[UDP Sender] 已关闭" << std::endl;
}

bool udp_sender::send_status(RecognitionStatus status, const std::string& username) {
    if (!initialized) {
        std::cerr << "[UDP Sender] 未初始化" << std::endl;
        return false;
    }

    // 构建数据包
    UdpStatusPacket packet;
    packet.magic_number = MAGIC_NUMBER;
    packet.version = PROTOCOL_VERSION;
    packet.status_code = static_cast<int32_t>(status);

    // 复制用户名（截断到63字符）
    memset(packet.username, 0, sizeof(packet.username));
    if (!username.empty()) {
        strncpy_s(packet.username, sizeof(packet.username), username.c_str(), _TRUNCATE);
    }

    // 设置时间戳
    packet.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // 发送数据包
    int sent_bytes = sendto(sock,
                           reinterpret_cast<const char*>(&packet),
                           sizeof(packet),
                           0,
                           reinterpret_cast<sockaddr*>(&server_addr),
                           sizeof(server_addr));

    if (sent_bytes == SOCKET_ERROR) {
        std::cerr << "[UDP Sender] 发送失败: " << WSAGetLastError() << std::endl;
        return false;
    }

    std::cout << "[UDP Sender] 发送状态: " << static_cast<int>(status)
              << ", 用户: " << (username.empty() ? "(无)" : username) << std::endl;

    return true;
}

void udp_sender::start_auto_send() {
    if (auto_send_running) {
        std::cout << "[UDP Sender] 自动发送已在运行" << std::endl;
        return;
    }

    auto_send_running = true;
    send_thread = std::thread(&udp_sender::auto_send_loop, this);
    std::cout << "[UDP Sender] 自动发送线程已启动" << std::endl;
}

void udp_sender::stop_auto_send() {
    if (!auto_send_running) {
        return;
    }

    auto_send_running = false;

    // 等待线程完成
    if (send_thread.joinable()) {
        send_thread.join();
    }

    std::cout << "[UDP Sender] 自动发送线程已停止" << std::endl;
}

void udp_sender::auto_send_loop() {
    RecognitionStatus last_status = RecognitionStatus::IDLE;

    while (auto_send_running) {
        RecognitionStatus current_status = udp_status_code.load();

        // 只有状态改变时才发送
        if (current_status != last_status) {
            send_status(current_status);
            last_status = current_status;
        }

        // 休眠100ms
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[UDP Sender] 自动发送循环结束" << std::endl;
}
