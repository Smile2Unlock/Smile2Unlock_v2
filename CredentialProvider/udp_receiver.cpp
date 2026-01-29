#include "udp_receiver.h"
#include <cstring>
#include <chrono>

udp_receiver::udp_receiver() : sock(INVALID_SOCKET), initialized(false) {
    // 初始化 Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "[UDP Receiver] WSAStartup 失败: " << result << std::endl;
        return;
    }

    // 创建 UDP socket
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "[UDP Receiver] 创建 socket 失败: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return;
    }

    // 设置 socket 为非阻塞模式（超时1秒）
    DWORD timeout = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    // 绑定到本地地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "[UDP Receiver] 绑定失败: " << WSAGetLastError() << std::endl;
        closesocket(sock);
        sock = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    initialized = true;
    std::cout << "[UDP Receiver] 初始化成功，监听端口: " << UDP_PORT << std::endl;

    // 自动启动接收线程
    start();
}

udp_receiver::~udp_receiver() {
    stop();

    if (sock != INVALID_SOCKET) {
        closesocket(sock);
    }
    WSACleanup();

    std::cout << "[UDP Receiver] 已关闭" << std::endl;
}

void udp_receiver::start() {
    if (!initialized) {
        std::cerr << "[UDP Receiver] 未初始化" << std::endl;
        return;
    }

    if (recv_running) {
        std::cout << "[UDP Receiver] 接收线程已在运行" << std::endl;
        return;
    }

    recv_running = true;
    recv_thread = std::thread(&udp_receiver::receive_loop, this);
    std::cout << "[UDP Receiver] 接收线程已启动" << std::endl;
}

void udp_receiver::stop() {
    if (!recv_running) {
        return;
    }

    recv_running = false;

    // 等待接收线程完成
    if (recv_thread.joinable()) {
        recv_thread.join();
    }

    std::cout << "[UDP Receiver] 接收线程已停止" << std::endl;
}

void udp_receiver::set_callback(std::function<void(RecognitionStatus, const std::string&)> callback) {
    status_callback = callback;
}

void udp_receiver::receive_loop() {
    UdpStatusPacket packet;
    sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);

    std::cout << "[UDP Receiver] 接收循环开始" << std::endl;

    while (recv_running) {
        // 接收数据包
        int recv_bytes = recvfrom(sock,
                                 reinterpret_cast<char*>(&packet),
                                 sizeof(packet),
                                 0,
                                 reinterpret_cast<sockaddr*>(&client_addr),
                                 &client_addr_len);

        if (recv_bytes == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAETIMEDOUT) {
                // 超时是正常的，继续循环
                continue;
            } else {
                std::cerr << "[UDP Receiver] 接收错误: " << error << std::endl;
                continue;
            }
        }

        // 验证数据包大小
        if (recv_bytes != sizeof(UdpStatusPacket)) {
            std::cerr << "[UDP Receiver] 数据包大小不匹配: " << recv_bytes << std::endl;
            continue;
        }

        // 验证魔术字
        if (packet.magic != MAGIC_NUMBER) {
            std::cerr << "[UDP Receiver] 无效的魔术字: 0x" << std::hex << packet.magic << std::dec << std::endl;
            continue;
        }

        // 验证协议版本
        if (packet.version != PROTOCOL_VERSION) {
            std::cerr << "[UDP Receiver] 协议版本不匹配: " << packet.version << std::endl;
            continue;
        }

        // 更新全局状态码
        int old_status = face_recognition_status.exchange(packet.status_code);

        // 更新用户名
        if (packet.username[0] != '\0') {
            recognized_username = std::string(packet.username);
        }

        std::cout << "[UDP Receiver] 收到状态更新: " << packet.status_code
                  << " (旧状态: " << old_status << ")"
                  << ", 用户: " << (packet.username[0] ? packet.username : "(无)") << std::endl;

        // 调用回调函数
        if (status_callback) {
            status_callback(static_cast<RecognitionStatus>(packet.status_code),
                          std::string(packet.username));
        }
    }

    std::cout << "[UDP Receiver] 接收循环结束" << std::endl;
}
