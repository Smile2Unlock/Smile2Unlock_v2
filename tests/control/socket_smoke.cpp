#include <unistd.h>
#include <sys/stat.h>

import std;
import su.control.socket;

int main() {
    const auto socket_path = std::filesystem::path("build/test-data/control")
        / std::format("socket-smoke-{}.sock", ::getpid());
    std::filesystem::create_directories(socket_path.parent_path());

    auto listener = su::control::Listener::bind_to(socket_path.string());
    if (!listener) {
        return 1;
    }
    struct stat metadata {};
    if (::stat(socket_path.c_str(), &metadata) != 0
        || (metadata.st_mode & 0777) != 0666) {
        return 1;
    }

    auto server_ok = std::atomic<bool>{false};
    auto server = std::jthread([&] {
        auto connection = listener->accept_one();
        if (!connection) {
            return;
        }
        auto request = connection->receive_frame();
        if (!request || !request->contains(R"("msg_type":"authenticate")")) {
            return;
        }
        server_ok.store(
            connection->send_frame(su::control::make_response(
                7,
                su::control::ControlResult::kAccepted,
                "smoke test",
                R"({"protection":"host-key"})"))
                .has_value(),
            std::memory_order_release);
    });

    auto client = su::control::Connection::connect_to(socket_path.string());
    if (!client
        || !client->send_frame(su::control::make_authenticate_request(7, "test-user"))) {
        return 1;
    }
    const auto response = client->receive_frame();
    if (!response) {
        return 1;
    }
    const auto result = su::control::parse_response(*response);
    server.join();
    return result && result->request_id == 7
            && result->result == su::control::ControlResult::kAccepted
            && result->reason == "smoke test"
            && result->payload_json == R"({"protection":"host-key"})"
            && su::control::make_enroll_profile_request(
                8, "test-user", "Front", "embedding:1,0", std::string(64, 'a'))
                .contains(R"("msg_type":"enroll_profile")")
            && su::control::make_issue_management_capability_request(
                10, 1000, static_cast<std::uint32_t>(::getpid()), "enroll_profile")
                .contains(R"("msg_type":"issue_management_capability")")
            && su::control::make_verify_profile_request(
                9, "test-user", "embedding:1,0", false)
                .contains(R"("liveness_ok":false)")
            && server_ok.load(std::memory_order_acquire)
        ? 0
        : 1;
}
