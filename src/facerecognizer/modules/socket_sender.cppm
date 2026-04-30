module;

#include <expected>

#include "../protocol/messages.h"

export module su.facerecognizer.socket_sender;

import std;

export namespace su::facerecognizer {

enum class SocketError { kConnectionRefused, kDisconnected, kSendFailed };

class SocketSender {
public:
    explicit SocketSender(std::string endpoint) : endpoint_(std::move(endpoint)) {}

    std::expected<void, SocketError> connect() {
        if (endpoint_.empty()) {
            return std::unexpected(SocketError::kConnectionRefused);
        }
        connected_ = true;
        return {};
    }

    std::expected<void, SocketError> send(const protocol::OutboundMessage& message) {
        if (!connected_) {
            return std::unexpected(SocketError::kDisconnected);
        }
        last_message_ = message;
        return {};
    }

    const std::optional<protocol::OutboundMessage>& last_message() const {
        return last_message_;
    }

private:
    std::string endpoint_;
    bool connected_ = false;
    std::optional<protocol::OutboundMessage> last_message_;
};

}  // namespace su::facerecognizer
