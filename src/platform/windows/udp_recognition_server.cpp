// Plain (non-module) TU: winsock2.h is safe to include here.
// See udp_recognition_server.h for the wire protocol.

#include "udp_recognition_server.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#pragma pack(push, 1)
struct UdpAuthRequestPacket {
    uint32_t magic_number;    // "AUTH"
    uint32_t version;
    int32_t request_type;
    char username_hint[64];
    uint64_t timestamp;
    uint32_t session_id;
};

struct UdpStatusPacket {
    uint32_t magic_number;    // 0x8581DAF3
    uint32_t version;
    int32_t status_code;
    uint32_t session_id;
    uint32_t feature_bytes;
    char username[64];
    char feature[48 * 1024];
    uint64_t timestamp;
};
#pragma pack(pop)

namespace {

std::atomic<bool> g_running{false};
std::atomic<bool> g_stop_requested{false};
std::thread g_server_thread;
SuUdpRecognizeCallback g_callback = nullptr;
void* g_userdata = nullptr;

uint64_t unix_time_millis() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

uint32_t read_le_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

int32_t read_le_i32(const uint8_t* p) {
    return static_cast<int32_t>(read_le_u32(p));
}

uint64_t read_le_u64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | p[i];
    }
    return value;
}

void server_loop() {
    WSADATA wsa_data{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return;
    }
    const auto cleanup_wsa = [](WSADATA*) { ::WSACleanup(); };
    std::unique_ptr<WSADATA, decltype(cleanup_wsa)> wsa_guard(&wsa_data, cleanup_wsa);

    SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return;
    }
    const auto close_sock = [](SOCKET* s) { ::closesocket(*s); };
    std::unique_ptr<SOCKET, decltype(close_sock)> sock_guard(&sock, close_sock);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    local.sin_port = ::htons(SU_UDP_AUTH_REQUEST_PORT);
    if (::bind(sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        return;
    }

    // Non-blocking receive so the loop can observe the stop flag.
    u_long nonblocking = 1;
    ::ioctlsocket(sock, FIONBIO, &nonblocking);

    uint8_t buffer[256] = {};
    while (!g_stop_requested.load(std::memory_order_acquire)) {
        sockaddr_in peer{};
        int peer_len = static_cast<int>(sizeof(peer));
        const int n = ::recvfrom(
            sock,
            reinterpret_cast<char*>(buffer),
            sizeof(buffer),
            0,
            reinterpret_cast<sockaddr*>(&peer),
            &peer_len);
        if (n == SOCKET_ERROR) {
            const int error = ::WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (n < 20) {
            continue;
        }
        const uint32_t magic = read_le_u32(buffer);
        const uint32_t version = read_le_u32(buffer + 4);
        const int32_t request_type = read_le_i32(buffer + 8);
        if (magic != SU_UDP_AUTH_REQUEST_MAGIC
            || version != SU_UDP_AUTH_REQUEST_VERSION
            || request_type != SU_UDP_REQ_START_RECOGNITION) {
            continue;
        }
        const uint32_t session_id = read_le_u32(buffer + 84);

        char username_hint[SU_UDP_STATUS_USERNAME_CAP] = {};
        std::memcpy(username_hint, buffer + 12, sizeof(username_hint) - 1);

        char out_username[SU_UDP_STATUS_USERNAME_CAP] = {};
        int status_code = SU_RS_RECOGNITION_ERROR;
        if (g_callback != nullptr) {
            status_code = g_callback(session_id, username_hint, out_username, g_userdata);
        }

        UdpStatusPacket reply{};
        reply.magic_number = SU_UDP_STATUS_MAGIC;
        reply.version = SU_UDP_STATUS_VERSION;
        reply.status_code = status_code;
        reply.session_id = session_id;
        reply.feature_bytes = 0;
        std::memcpy(reply.username, out_username, sizeof(reply.username));
        reply.timestamp = unix_time_millis();

        sockaddr_in status_peer{};
        status_peer.sin_family = AF_INET;
        status_peer.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        status_peer.sin_port = ::htons(SU_UDP_CP_STATUS_PORT);
        (void)::sendto(
            sock,
            reinterpret_cast<const char*>(&reply),
            sizeof(reply),
            0,
            reinterpret_cast<const sockaddr*>(&status_peer),
            sizeof(status_peer));
    }
}

}  // namespace

extern "C" {

int su_udp_start_recognition_server(
    SuUdpRecognizeCallback callback,
    void* userdata) {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) {
        return 0;  // already running
    }
    g_stop_requested.store(false, std::memory_order_release);
    g_callback = callback;
    g_userdata = userdata;
    try {
        g_server_thread = std::thread(server_loop);
    } catch (...) {
        g_running.store(false, std::memory_order_release);
        return -1;
    }
    return 0;
}

void su_udp_stop_recognition_server(void) {
    g_stop_requested.store(true, std::memory_order_release);
    if (g_server_thread.joinable()) {
        g_server_thread.join();
        g_server_thread = std::thread();
    }
    g_running.store(false, std::memory_order_release);
    g_callback = nullptr;
    g_userdata = nullptr;
}

}  // extern "C"
