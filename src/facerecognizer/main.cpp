#include "protocol/messages.h"
#include "seetaface.h"

import std;
import su.facerecognizer.camera;
import su.facerecognizer.pipeline;
import su.facerecognizer.socket_sender;

namespace su::facerecognizer {

namespace {

std::string default_endpoint() {
#if defined(_WIN32)
    return "127.0.0.1:43101";
#else
    return "/run/smile2unlock/fr.sock";
#endif
}

protocol::OutboundMessage make_boot_message() {
    protocol::OutboundMessage message;
    message.type = protocol::MessageType::kError;
    message.error_message = "su_facerecognizer bootstrap placeholder";
    return message;
}

}  // namespace

int run(std::span<char*> args) {
    (void)args;

    SocketSender sender(default_endpoint());
    if (!sender.connect()) {
        std::cerr << "failed to connect to su_app endpoint\n";
        return 1;
    }

    sender.send(make_boot_message());

    try {
        CameraCapture camera(0);
        SeetaFaceEngine engine;
        RecognitionPipeline pipeline(camera, engine);
        (void)pipeline;
    } catch (const std::exception& exception) {
        std::cerr << "su_facerecognizer init failed: " << exception.what() << '\n';
        return 2;
    }

    return 0;
}

}  // namespace su::facerecognizer

int main(int argc, char** argv) {
    return su::facerecognizer::run(std::span(argv, static_cast<std::size_t>(argc)));
}
