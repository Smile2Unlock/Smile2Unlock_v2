#include "rewrite/backend_core.hpp"

#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <thread>
#include <atomic>
#include <memory>
#include <optional>

import rewrite.protocol;
import rewrite.runtime;
import rewrite.socket;

namespace {

using namespace rewrite::ipc;
using namespace rewrite::net;
using namespace rewrite::runtime;

constexpr std::uint16_t kFrClientVersion = 1;

class FrClient {
public:
    FrClient() = default;

    int run() {
        std::cout << "[su_facerecognizer] worker skeleton online\n";
        std::cout << "[su_facerecognizer] connecting to backend at " << backend_endpoints_.fr_control << "\n";

        if (auto r = connect_to_backend(); !r) {
            std::cerr << "[su_facerecognizer] failed to connect: " << r.error() << "\n";
            return 1;
        }

        std::cout << "[su_facerecognizer] connected to backend\n";

        send_hello();

        while (running_) {
            if (!receive_and_process()) {
                break;
            }
        }

        disconnect();
        return 0;
    }

    void stop() {
        running_ = false;
    }

private:
    [[nodiscard]] std::expected<void, std::string> connect_to_backend() {
        SocketClient client;
        if (auto r = client.connect(ListenEndpoint{{backend_endpoints_.fr_control}}); !r) {
            return std::unexpected(r.error());
        }
        connection_ = SocketConnection(client.native_handle());
        return {};
    }

    void disconnect() {
        connection_.close();
    }

    void send_hello() {
        FrameHeaderV1 header{};
        header.msg_type = static_cast<std::uint16_t>(MessageType::fr_command);
        header.flags = flag_value(Flag::request);
        header.request_id = next_request_id_++;

        std::string body = std::format(
            R"({{"command":"HELLO","version":{}}})",
            kFrClientVersion);

        header.body_bytes = static_cast<std::uint32_t>(body.size());
        header.body_codec = static_cast<std::uint16_t>(BodyCodec::utf8_json);

        (void)connection_.send_header(header);
        (void)connection_.send_bytes(std::as_bytes(std::span(body)));
    }

    bool receive_and_process() {
        auto header_bytes = connection_.receive_bytes(sizeof(FrameHeaderV1));
        if (!header_bytes) {
            return false;
        }

        auto header = parse_header(std::as_bytes(std::span(*header_bytes)));
        if (!header) {
            std::cerr << "[su_facerecognizer] bad header: " << header.error() << "\n";
            return false;
        }

        std::vector<std::byte> body;
        if (header->body_bytes > 0) {
            auto body_result = connection_.receive_all(header->body_bytes);
            if (!body_result) {
                return false;
            }
            body = std::move(*body_result);
        }

        process_frame(*header, body);
        return true;
    }

    void process_frame(const FrameHeaderV1& header, std::span<const std::byte> body) {
        auto msg_type = static_cast<MessageType>(header.msg_type);

        switch (msg_type) {
            case MessageType::fr_command:
                process_fr_command(header, body);
                break;
            case MessageType::error_message:
                process_error(header, body);
                break;
            default:
                std::cerr << "[su_facerecognizer] unexpected message type: " << static_cast<int>(msg_type) << "\n";
                break;
        }
    }

    void process_fr_command(const FrameHeaderV1& header, std::span<const std::byte> body) {
        std::string body_str(reinterpret_cast<const char*>(body.data()), body.size());
        std::cout << "[su_facerecognizer] fr_command received: " << body_str << "\n";

        FrameHeaderV1 resp = header;
        resp.msg_type = static_cast<std::uint16_t>(MessageType::fr_response);
        resp.flags = flag_value(Flag::response);
        resp.status = static_cast<std::uint32_t>(StatusCode::ok);

        std::string response_body = R"({"status":"acknowledged"})";
        resp.body_bytes = static_cast<std::uint32_t>(response_body.size());
        resp.body_codec = static_cast<std::uint16_t>(BodyCodec::utf8_json);

        (void)connection_.send_header(resp);
        (void)connection_.send_bytes(std::as_bytes(std::span(response_body)));
    }

    void process_error(const FrameHeaderV1& header, std::span<const std::byte> body) {
        std::string body_str(reinterpret_cast<const char*>(body.data()), body.size());
        std::cerr << "[su_facerecognizer] error: " << body_str << "\n";
    }

    EndpointSet backend_endpoints_{default_endpoints()};
    SocketConnection connection_;
    std::atomic<bool> running_{true};
    std::uint64_t next_request_id_{1};
};

} // namespace

int main() {
    std::cout << "[su_facerecognizer] starting...\n";

    rewrite::backend::BackendCore backend;
    if (auto init = backend.initialize(); !init) {
        std::cerr << "[su_facerecognizer] initialize failed: " << init.error() << '\n';
        return 1;
    }

    FrClient client;
    int result = client.run();

    std::cout << "[su_facerecognizer] exiting with code " << result << "\n";
    return result;
}