#include <security/pam_appl.h>
#include <unistd.h>

import std;
import su.control.socket;

#ifndef SU_PAM_MODULE_PATH
#error "SU_PAM_MODULE_PATH must point to the built PAM module"
#endif

namespace {

constexpr auto kServiceName = std::string_view{"smile2unlock-test"};

class PamGuard {
public:
    explicit PamGuard(pam_handle_t* handle) : handle_(handle) {}
    ~PamGuard() { (void)::pam_end(handle_, PAM_SUCCESS); }
    PamGuard(const PamGuard&) = delete;
    PamGuard& operator=(const PamGuard&) = delete;

private:
    pam_handle_t* handle_;
};

int reject_conversation(
    int message_count,
    const pam_message** messages,
    pam_response** responses,
    void* app_data) {
    (void)message_count;
    (void)messages;
    (void)responses;
    (void)app_data;
    return PAM_CONV_ERR;
}

std::filesystem::path case_directory(std::string_view name) {
    return std::filesystem::path("build/test-data/pam")
        / std::format("{}-{}", name, ::getpid());
}

bool write_service_config(
    const std::filesystem::path& directory,
    const std::filesystem::path& socket_path) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }
    auto config = std::ofstream(directory / std::string(kServiceName));
    config << "auth required " << SU_PAM_MODULE_PATH
           << " socket=" << socket_path.string() << '\n';
    return config.good();
}

std::optional<std::uint64_t> request_id_from(std::string_view request) {
    constexpr auto field = std::string_view{R"("request_id":)"};
    const auto start = request.find(field);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const auto digits = request.substr(start + field.size());
    auto request_id = std::uint64_t{0};
    const auto parsed = std::from_chars(
        digits.data(), digits.data() + digits.size(), request_id);
    return parsed.ec == std::errc{}
            && parsed.ptr != digits.data()
            && parsed.ptr != digits.data() + digits.size()
            && *parsed.ptr == ','
            && request_id != 0
        ? std::optional{request_id}
        : std::nullopt;
}

std::expected<pam_handle_t*, int> start_pam(const std::filesystem::path& config_directory) {
    static const auto conversation = pam_conv{
        .conv = reject_conversation,
        .appdata_ptr = nullptr,
    };
    auto* handle = static_cast<pam_handle_t*>(nullptr);
    const auto status = ::pam_start_confdir(
        kServiceName.data(),
        "test-user",
        &conversation,
        config_directory.c_str(),
        &handle);
    if (status != PAM_SUCCESS) {
        return std::unexpected(status);
    }
    return handle;
}

bool run_socket_case(
    std::string_view name,
    su::control::ControlResult result,
    int expected_pam_status,
    bool matching_request_id = true) {
    const auto directory = case_directory(name);
    const auto socket_path = directory / "control.sock";
    if (!write_service_config(directory, socket_path)) {
        return false;
    }
    auto pam = start_pam(directory);
    if (!pam) {
        return false;
    }
    const auto end_pam = PamGuard{*pam};

    auto listener = su::control::Listener::bind_to(socket_path.string());
    if (!listener) {
        return false;
    }
    auto server_ok = std::atomic<bool>{false};
    auto server = std::jthread([&] {
        auto connection = listener->accept_one();
        if (!connection) {
            return;
        }
        const auto request = connection->receive_frame();
        if (!request || !request->contains(R"("username":"test-user")")) {
            return;
        }
        const auto request_id = request_id_from(*request);
        if (!request_id) {
            return;
        }
        const auto response_id = matching_request_id ? *request_id : *request_id + 1;
        server_ok.store(
            connection->send_frame(su::control::make_response(
                response_id, result, "PAM integration test"))
                .has_value(),
            std::memory_order_release);
    });

    const auto status = ::pam_authenticate(*pam, 0);
    server.join();
    return status == expected_pam_status
        && server_ok.load(std::memory_order_acquire);
}

bool run_unavailable_case() {
    const auto directory = case_directory("unavailable");
    const auto socket_path = directory / "missing.sock";
    if (!write_service_config(directory, socket_path)) {
        return false;
    }
    auto pam = start_pam(directory);
    if (!pam) {
        return false;
    }
    const auto status = ::pam_authenticate(*pam, 0);
    (void)::pam_end(*pam, status);
    return status == PAM_AUTHINFO_UNAVAIL;
}

} // namespace

int main() {
    return run_socket_case(
               "accepted", su::control::ControlResult::kAccepted, PAM_SUCCESS)
            && run_socket_case(
                "rejected", su::control::ControlResult::kRejected, PAM_AUTH_ERR)
            && run_socket_case(
                "mismatched-id",
                su::control::ControlResult::kAccepted,
                PAM_AUTHINFO_UNAVAIL,
                false)
            && run_unavailable_case()
        ? 0
        : 1;
}
