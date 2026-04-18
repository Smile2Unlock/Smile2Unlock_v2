module;

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

export module rewrite.socket;

import std;
import rewrite.protocol;

export namespace rewrite::net {

#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

struct ListenEndpoint {
    std::string value;
};

[[nodiscard]] inline bool is_tcp_endpoint(const std::string_view value) {
    return value.contains(':');
}

class SocketServer {
public:
    SocketServer() = default;

    explicit SocketServer(ListenEndpoint endpoint)
        : endpoint_(std::move(endpoint)) {}

    ~SocketServer() {
        close();
    }

    SocketServer(const SocketServer&) = delete;
    SocketServer& operator=(const SocketServer&) = delete;

    SocketServer(SocketServer&& other) noexcept
        : endpoint_(std::move(other.endpoint_))
        , handle_(std::exchange(other.handle_, kInvalidSocket)) {}

    SocketServer& operator=(SocketServer&& other) noexcept {
        if (this != &other) {
            close();
            endpoint_ = std::move(other.endpoint_);
            handle_ = std::exchange(other.handle_, kInvalidSocket);
        }
        return *this;
    }

    [[nodiscard]] std::expected<void, std::string> bind_and_listen() {
        close();
#ifdef _WIN32
        return std::unexpected("windows transport not implemented in this milestone");
#else
        if (endpoint_.value.empty()) {
            return std::unexpected("empty endpoint");
        }

        if (is_tcp_endpoint(endpoint_.value)) {
            return std::unexpected("tcp endpoints are reserved for windows in this milestone");
        }

        const auto parent = std::filesystem::path(endpoint_.value).parent_path();
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        std::filesystem::remove(endpoint_.value, ec);

        handle_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (handle_ == kInvalidSocket) {
            return std::unexpected(std::format("socket() failed: {}", std::strerror(errno)));
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", endpoint_.value.c_str());
        if (::bind(handle_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            const std::string error = std::format("bind() failed: {}", std::strerror(errno));
            close();
            return std::unexpected(error);
        }

        if (::listen(handle_, 16) != 0) {
            const std::string error = std::format("listen() failed: {}", std::strerror(errno));
            close();
            return std::unexpected(error);
        }
        return {};
#endif
    }

    [[nodiscard]] SocketHandle native_handle() const noexcept {
        return handle_;
    }

    void close() noexcept {
#ifdef _WIN32
        if (handle_ != kInvalidSocket) {
            closesocket(handle_);
            handle_ = kInvalidSocket;
        }
#else
        if (handle_ != kInvalidSocket) {
            ::close(handle_);
            handle_ = kInvalidSocket;
        }
#endif
    }

private:
    ListenEndpoint endpoint_{};
    SocketHandle handle_{kInvalidSocket};
};

class SocketClient {
public:
    SocketClient() = default;

    ~SocketClient() {
        close();
    }

    [[nodiscard]] std::expected<void, std::string> connect(const ListenEndpoint& endpoint) {
        close();
#ifdef _WIN32
        return std::unexpected("windows transport not implemented in this milestone");
#else
        if (endpoint.value.empty()) {
            return std::unexpected("empty endpoint");
        }
        if (is_tcp_endpoint(endpoint.value)) {
            return std::unexpected("tcp endpoints are reserved for windows in this milestone");
        }

        handle_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (handle_ == kInvalidSocket) {
            return std::unexpected(std::format("socket() failed: {}", std::strerror(errno)));
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", endpoint.value.c_str());
        if (::connect(handle_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            const std::string error = std::format("connect() failed: {}", std::strerror(errno));
            close();
            return std::unexpected(error);
        }
        return {};
#endif
    }

    [[nodiscard]] std::expected<void, std::string> send_header(const ipc::FrameHeaderV1& header) const {
        const auto bytes = ipc::serialize_header(header);
        return send_bytes(std::as_bytes(std::span(bytes)));
    }

    [[nodiscard]] std::expected<void, std::string> send_bytes(std::span<const std::byte> bytes) const {
        if (handle_ == kInvalidSocket) {
            return std::unexpected("socket not connected");
        }

        std::size_t offset = 0;
        while (offset < bytes.size()) {
#ifdef _WIN32
            const int sent = ::send(handle_,
                                    reinterpret_cast<const char*>(bytes.data() + offset),
                                    static_cast<int>(bytes.size() - offset),
                                    0);
            if (sent == SOCKET_ERROR) {
                return std::unexpected("send() failed");
            }
#else
            const auto sent = ::send(handle_, bytes.data() + offset, bytes.size() - offset, 0);
            if (sent <= 0) {
                return std::unexpected(std::format("send() failed: {}", std::strerror(errno)));
            }
#endif
            offset += static_cast<std::size_t>(sent);
        }
        return {};
    }

    void close() noexcept {
#ifdef _WIN32
        if (handle_ != kInvalidSocket) {
            closesocket(handle_);
            handle_ = kInvalidSocket;
        }
#else
        if (handle_ != kInvalidSocket) {
            ::close(handle_);
            handle_ = kInvalidSocket;
        }
#endif
    }

private:
    SocketHandle handle_{kInvalidSocket};
};

} // namespace rewrite::net
