#include <arpa/inet.h>
#include <cstdio>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

import std;
import su.control.socket;

namespace {

using su::control::Connection;
using su::control::SocketError;
constexpr auto kBudget = std::chrono::milliseconds{200};

struct SocketGuard {
    int fd;
    ~SocketGuard() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

bool with_peer(std::string_view name, const auto& operation) {
    const auto path = std::filesystem::path{"build/test-data/control"} /
                      std::format("deadline-{}-{}.sock", name, ::getpid());
    std::filesystem::create_directories(path.parent_path());
    auto listener = su::control::Listener::bind_to(path.string());
    const auto peer = SocketGuard{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!listener || peer.fd < 0) {
        return false;
    }
    auto address = sockaddr_un{};
    address.sun_family = AF_UNIX;
    const auto text = path.string();
    if (text.size() >= sizeof(address.sun_path)) {
        return false;
    }
    std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
    if (::connect(peer.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return false;
    }
    auto connection = listener->accept_one(kBudget);
    return connection && operation(*connection, peer.fd);
}

bool trickled_frame(std::string_view name, bool slow_header, bool delay_header) {
    return with_peer(name, [=](Connection& connection, int peer) {
        const auto length = ::htonl(4);
        auto writer = std::jthread([=] {
            if (delay_header) {
                std::this_thread::sleep_for(std::chrono::milliseconds{120});
            }
            if (slow_header) {
                const auto* bytes = reinterpret_cast<const char*>(&length);
                for (auto i = 0; i < 4; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{70});
                    (void)::send(peer, bytes + i, 1, MSG_NOSIGNAL);
                }
            } else {
                (void)::send(peer, &length, sizeof(length), MSG_NOSIGNAL);
            }
            for (auto i = 0; i < 4; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds{70});
                (void)::send(peer, "x", 1, MSG_NOSIGNAL);
            }
        });
        // Both move operations must retain the selected timeout.
        auto moved = Connection{std::move(connection)};
        auto receiver = Connection{};
        receiver = std::move(moved);
        const auto start = std::chrono::steady_clock::now();
        const auto result = receiver.receive_frame();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        return !result && result.error() == SocketError::kTimeout && elapsed >= kBudget &&
               elapsed < std::chrono::seconds{1};
    });
}

bool blocked_writer() {
    return with_peer("backpressure", [](Connection& connection, int) {
        const auto payload = std::string(su::control::kMaximumFrameSize, 'x');
        const auto result = connection.send_frame(payload);
        return !result && result.error() == SocketError::kTimeout;
    });
}

bool healthy_peer() {
    return with_peer("healthy", [](Connection& connection, int peer) {
        const auto length = ::htonl(2);
        if (::send(peer, &length, sizeof(length), MSG_NOSIGNAL) !=
                static_cast<ssize_t>(sizeof(length)) ||
            ::send(peer, "ok", 2, MSG_NOSIGNAL) != 2) {
            return false;
        }
        const auto received = connection.receive_frame();
        return received && *received == "ok" && connection.send_frame("ok").has_value();
    });
}

bool disconnected_peer() {
    return with_peer("eof", [](Connection& connection, int peer) {
        (void)::shutdown(peer, SHUT_WR);
        const auto result = connection.receive_frame();
        return !result && result.error() == SocketError::kReadFailed;
    });
}

} // namespace

int main() {
    if (!trickled_frame("header", true, false) || !trickled_frame("body", false, false) ||
        !trickled_frame("shared-budget", false, true) || !blocked_writer() || !healthy_peer() ||
        !disconnected_peer() || Connection::connect_to("unused", std::chrono::milliseconds{0}) ||
        Connection::connect_to("unused", std::chrono::milliseconds{-1})) {
        std::println(stderr, "socket frame deadline, backpressure or recovery check failed");
        return 1;
    }
    std::println("socket frame deadline, backpressure and recovery: ok");
}
