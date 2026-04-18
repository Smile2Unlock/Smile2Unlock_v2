#include "rewrite/backend_core.hpp"

#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <utility>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_map>

import rewrite.protocol;
import rewrite.runtime;
import rewrite.socket;

namespace {

using namespace rewrite::ipc;
using namespace rewrite::net;
using namespace rewrite::runtime;

constexpr std::size_t kMaxFdSetsize = 1024;

class EventBroadcaster {
public:
    void add_subscriber(SocketConnection conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers_.push_back(std::move(conn));
    }

    void broadcast(const FrameHeaderV1& header, std::span<const std::byte> body) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& conn : subscribers_) {
            if (conn) {
                (void)conn.send_header(header);
                if (!body.empty()) {
                    (void)conn.send_bytes(body);
                }
            }
        }
    }

    void remove_dead_subscribers() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::erase_if(subscribers_, [](const SocketConnection& conn) {
            return !conn;
        });
    }

private:
    std::mutex mutex_;
    std::vector<SocketConnection> subscribers_;
};

class ControlHandler {
public:
    explicit ControlHandler(rewrite::backend::BackendCore& backend, EventBroadcaster& events)
        : backend_(backend), events_(events) {}

    void handle_connection(SocketConnection conn) {
        while (conn) {
            auto header_bytes = conn.receive_bytes(sizeof(FrameHeaderV1));
            if (!header_bytes) {
                break;
            }

            auto header = parse_header(std::as_bytes(std::span(*header_bytes)));
            if (!header) {
                send_error(conn, 0, header.error());
                break;
            }

            if (header->msg_type != static_cast<std::uint16_t>(MessageType::control_request)) {
                send_error(conn, header->request_id, "expected control_request");
                break;
            }

            if (header->body_bytes > kMaxControlBodyBytes) {
                send_error(conn, header->request_id, "body too large");
                break;
            }

            std::vector<std::byte> body;
            if (header->body_bytes > 0) {
                auto body_result = conn.receive_all(header->body_bytes);
                if (!body_result) {
                    break;
                }
                body = std::move(*body_result);
            }

            process_request(conn, *header, std::move(body));
        }
    }

private:
    void process_request(SocketConnection& conn, const FrameHeaderV1& header, std::vector<std::byte> body) {
        auto response = process_json_command(header, body);

        FrameHeaderV1 resp_header = header;
        resp_header.msg_type = static_cast<std::uint16_t>(MessageType::control_response);
        resp_header.flags = flag_value(Flag::response);
        resp_header.body_bytes = static_cast<std::uint32_t>(response.size());
        resp_header.body_codec = static_cast<std::uint16_t>(BodyCodec::utf8_json);

        (void)conn.send_header(resp_header);
        if (!response.empty()) {
            (void)conn.send_bytes(std::as_bytes(std::span(response)));
        }
    }

    [[nodiscard]] std::string process_json_command(const FrameHeaderV1& header, std::span<const std::byte> body) {
        std::string_view body_str(reinterpret_cast<const char*>(body.data()), body.size());

        if (body_str == "{}" || body_str.empty()) {
            return handle_empty_command(header.request_id);
        }

        return std::format(R"({{"error":"parse error - expected JSON object"}})");
    }

    [[nodiscard]] std::string handle_empty_command(std::uint64_t request_id) {
        return std::format(R"({{"request_id":{},"error":"empty request"}})", request_id);
    }

    void send_error(SocketConnection& conn, std::uint64_t request_id, std::string_view message) {
        FrameHeaderV1 resp{};
        resp.msg_type = static_cast<std::uint16_t>(MessageType::error_message);
        resp.flags = flag_value(Flag::error);
        resp.status = static_cast<std::uint32_t>(StatusCode::bad_payload);
        resp.request_id = request_id;

        std::string body = std::format(R"({{"error":"{}"}})", message);
        resp.body_bytes = static_cast<std::uint32_t>(body.size());
        resp.body_codec = static_cast<std::uint16_t>(BodyCodec::utf8_json);

        (void)conn.send_header(resp);
        (void)conn.send_bytes(std::as_bytes(std::span(body)));
    }

    rewrite::backend::BackendCore& backend_;
    EventBroadcaster& events_;
};

class EventChannelHandler {
public:
    explicit EventChannelHandler(EventBroadcaster& broadcaster) : broadcaster_(broadcaster) {}

    void handle_connection(SocketConnection conn) {
        broadcaster_.add_subscriber(std::move(conn));
    }

private:
    EventBroadcaster& broadcaster_;
};

class FrControlHandler {
public:
    explicit FrControlHandler(rewrite::backend::BackendCore& backend) : backend_(backend) {}

    void handle_connection(SocketConnection conn) {
        while (conn) {
            auto header_bytes = conn.receive_bytes(sizeof(FrameHeaderV1));
            if (!header_bytes) {
                break;
            }

            auto header = parse_header(std::as_bytes(std::span(*header_bytes)));
            if (!header) {
                break;
            }

            if (header->msg_type != static_cast<std::uint16_t>(MessageType::fr_command)) {
                break;
            }

            std::vector<std::byte> body;
            if (header->body_bytes > 0) {
                auto body_result = conn.receive_all(header->body_bytes);
                if (!body_result) {
                    break;
                }
                body = std::move(*body_result);
            }

            process_fr_command(conn, *header, body);
        }
    }

private:
    void process_fr_command(SocketConnection& conn, const FrameHeaderV1& header, std::span<const std::byte> body) {
        FrameHeaderV1 resp = header;
        resp.msg_type = static_cast<std::uint16_t>(MessageType::fr_response);
        resp.flags = flag_value(Flag::response);
        resp.status = static_cast<std::uint32_t>(StatusCode::ok);

        std::string response_body = R"({"status":"fr client not implemented"})";
        resp.body_bytes = static_cast<std::uint32_t>(response_body.size());
        resp.body_codec = static_cast<std::uint16_t>(BodyCodec::utf8_json);

        (void)conn.send_header(resp);
        (void)conn.send_bytes(std::as_bytes(std::span(response_body)));
    }

    rewrite::backend::BackendCore& backend_;
};

class PreviewHandler {
public:
    void handle_connection(SocketConnection conn) {
        while (conn) {
            auto header_bytes = conn.receive_bytes(sizeof(FrameHeaderV1));
            if (!header_bytes) {
                break;
            }
        }
    }
};

class IoContext {
public:
    IoContext() {
        FD_ZERO(&read_fds_);
    }

    void add_server(SocketHandle fd) {
        if (nfds_ <= fd) {
            nfds_ = fd + 1;
        }
        FD_SET(fd, &read_fds_);
        servers_.push_back(fd);
    }

    void add_client(SocketHandle fd) {
        if (nfds_ <= fd) {
            nfds_ = fd + 1;
        }
        FD_SET(fd, &read_fds_);
        clients_.push_back(fd);
    }

    bool wait(std::chrono::milliseconds timeout) {
        fd_set read_fds = read_fds_;
        timeval tv{};
        tv.tv_sec = static_cast<long>(timeout.count() / 1000);
        tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

        int ret = select(nfds_, &read_fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            return false;
        }
        return ret > 0;
    }

    [[nodiscard]] bool is_server_ready(SocketHandle fd) const {
        return FD_ISSET(fd, &read_fds_) != 0;
    }

    void close_clients() {
        for (auto fd : clients_) {
            if (fd >= 0) {
#ifdef _WIN32
                closesocket(fd);
#else
                ::close(fd);
#endif
            }
        }
        clients_.clear();
        FD_ZERO(&read_fds_);
        for (auto fd : servers_) {
            FD_SET(fd, &read_fds_);
        }
        nfds_ = 0;
        for (auto fd : servers_) {
            if (nfds_ <= fd) {
                nfds_ = fd + 1;
            }
        }
    }

private:
    fd_set read_fds_;
    std::vector<SocketHandle> servers_;
    std::vector<SocketHandle> clients_;
    int nfds_{0};
};

class EventLoop {
public:
    EventLoop(rewrite::backend::BackendCore& backend)
        : backend_(backend)
        , event_handler_(broadcaster_)
        , control_handler_(backend, broadcaster_)
        , fr_handler_(backend) {}

    int run() {
        std::cout << "[su_backendd] event loop started\n";

        const auto& eps = backend_.endpoints();

        SocketServer control_server{{eps.control}};
        SocketServer events_server{{eps.events}};
        SocketServer fr_server{{eps.fr_control}};
        SocketServer preview_server{{eps.preview}};

        if (auto r = control_server.bind_and_listen(); !r) {
            std::cerr << "[su_backendd] control listener failed: " << r.error() << '\n';
            return 1;
        }
        if (auto r = events_server.bind_and_listen(); !r) {
            std::cerr << "[su_backendd] events listener failed: " << r.error() << '\n';
            return 1;
        }
        if (auto r = fr_server.bind_and_listen(); !r) {
            std::cerr << "[su_backendd] fr-control listener failed: " << r.error() << '\n';
            return 1;
        }
        if (auto r = preview_server.bind_and_listen(); !r) {
            std::cerr << "[su_backendd] preview listener failed: " << r.error() << '\n';
            return 1;
        }

        IoContext ctx;
        ctx.add_server(control_server.native_handle());
        ctx.add_server(events_server.native_handle());
        ctx.add_server(fr_server.native_handle());
        ctx.add_server(preview_server.native_handle());

        std::cout << "[su_backendd] all listeners active\n";

        while (running_) {
            if (!ctx.wait(std::chrono::milliseconds(100))) {
                continue;
            }

            if (ctx.is_server_ready(control_server.native_handle())) {
                if (auto client = control_server.accept()) {
                    handle_control_client(SocketConnection(*client));
                }
            }
            if (ctx.is_server_ready(events_server.native_handle())) {
                if (auto client = events_server.accept()) {
                    handle_event_client(SocketConnection(*client));
                }
            }
            if (ctx.is_server_ready(fr_server.native_handle())) {
                if (auto client = fr_server.accept()) {
                    handle_fr_client(SocketConnection(*client));
                }
            }
            if (ctx.is_server_ready(preview_server.native_handle())) {
                if (auto client = preview_server.accept()) {
                    handle_preview_client(SocketConnection(*client));
                }
            }

            broadcaster_.remove_dead_subscribers();
        }

        ctx.close_clients();
        return 0;
    }

    void stop() { running_ = false; }

private:
    void handle_control_client(SocketConnection conn) {
        std::thread([this, conn = std::move(conn)]() mutable {
            ControlHandler handler(backend_, broadcaster_);
            handler.handle_connection(std::move(conn));
        }).detach();
    }

    void handle_event_client(SocketConnection conn) {
        event_handler_.handle_connection(std::move(conn));
    }

    void handle_fr_client(SocketConnection conn) {
        std::thread([this, conn = std::move(conn)]() mutable {
            FrControlHandler handler(backend_);
            handler.handle_connection(std::move(conn));
        }).detach();
    }

    void handle_preview_client(SocketConnection conn) {
        std::thread([](SocketConnection conn) mutable {
            PreviewHandler handler;
            handler.handle_connection(std::move(conn));
        }, std::move(conn)).detach();
    }

    bool running_{true};
    rewrite::backend::BackendCore& backend_;
    EventBroadcaster broadcaster_;
    EventChannelHandler event_handler_;
    ControlHandler control_handler_;
    FrControlHandler fr_handler_;
};

} // namespace

int main() {
    std::cout << "[su_backendd] starting...\n";

    rewrite::backend::BackendCore backend;
    if (auto init = backend.initialize(); !init) {
        std::cerr << "[su_backendd] initialize failed: " << init.error() << '\n';
        return 1;
    }
    std::cout << "[su_backendd] " << backend.status_json() << '\n';

    if (auto listeners = backend.start_listeners(); !listeners) {
        std::cerr << "[su_backendd] listener startup failed: " << listeners.error() << '\n';
        return 2;
    }
    std::cout << "[su_backendd] listeners active\n";

    EventLoop loop(backend);
    return loop.run();
}