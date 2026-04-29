module;

#include "../protocol/messages.h"

export module su.facerecognizer.socket_sender;

import std;

export namespace su::facerecognizer {

class SocketSender {
public:
    explicit SocketSender(std::string endpoint) : endpoint_(std::move(endpoint)) {}

    bool connect() {
        connected_ = !endpoint_.empty();
        return connected_;
    }

    bool send(const protocol::OutboundMessage& message) {
        if (!connected_) {
            return false;
        }
        last_message_ = message;
        return true;
    }

    const protocol::OutboundMessage& last_message() const {
        return last_message_;
    }

private:
    std::string endpoint_;
    bool connected_ = false;
    protocol::OutboundMessage last_message_{};
};

}  // namespace su::facerecognizer
