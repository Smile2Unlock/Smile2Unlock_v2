#include <security/pam_appl.h>
#include <unistd.h>
#include "platform/linux/deploy/deployment.h"

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

std::expected<pam_handle_t*, int> start_pam(
    const std::filesystem::path& config_directory,
    std::string_view service = kServiceName) {
    static const auto conversation = pam_conv{
        .conv = reject_conversation,
        .appdata_ptr = nullptr,
    };
    auto* handle = static_cast<pam_handle_t*>(nullptr);
    const auto status = ::pam_start_confdir(
        service.data(),
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
    bool matching_request_id = true,
    bool malformed_response = false) {
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
        const auto response = malformed_response
            ? std::format(
                R"({{"version":"2","msg_type":"auth_result","request_id":{},"result":"accepted","reason":""}})",
                response_id)
            : su::control::make_response(response_id, result, "PAM integration test");
        server_ok.store(
            connection->send_frame(response).has_value(),
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

bool run_timeout_case(std::string_view name, std::optional<bool> password_accepts) {
    const auto directory = case_directory(name);
    const auto socket_path = directory / "control.sock";
    std::filesystem::remove_all(directory);
    if (!write_service_config(directory, socket_path)) {
        return false;
    }
    if (password_accepts) {
        auto config = std::ofstream(directory / std::string(kServiceName));
        config << "auth sufficient " << SU_PAM_MODULE_PATH
               << " socket=" << socket_path.string() << '\n'
               << "auth required " << (*password_accepts ? "pam_permit.so" : "pam_deny.so")
               << '\n';
        if (!config.good()) {
            return false;
        }
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
    auto peer_closed = std::atomic<bool>{false};
    auto server = std::jthread([&] {
        // Stay silent after receiving the request. Bound the fixture itself,
        // so a broken client fails the test rather than hanging the suite.
        auto connection = listener->accept_one(std::chrono::seconds{12});
        if (!connection || !connection->receive_frame()) {
            return;
        }
        const auto response = connection->receive_frame();
        peer_closed.store(
            !response && response.error() == su::control::SocketError::kReadFailed);
    });
    const auto started = std::chrono::steady_clock::now();
    const auto status = ::pam_authenticate(*pam, 0);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    // Unblock accept even if PAM failed before opening its connection.
    // The temporary immediately closes, so a waiting fixture also sees EOF.
    (void)su::control::Connection::connect_to(socket_path.string(), std::chrono::milliseconds{100});
    server.join();
    const auto expected = password_accepts
        ? (*password_accepts ? PAM_SUCCESS : PAM_AUTH_ERR)
        : PAM_AUTHINFO_UNAVAIL;
    const auto passed = status == expected
        && elapsed >= std::chrono::seconds{7}
        && elapsed < std::chrono::seconds{9}
        && peer_closed.load();
    if (!passed) {
        std::cerr << name << ": expected " << expected << ", got " << status
                  << ", elapsed "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << " ms, peer closed " << peer_closed.load() << '\n';
    }
    return passed;
}

bool run_administrator_case(
    su::deploy::TargetKind target,
    std::optional<su::control::ControlResult> face_result,
    bool password_accepts,
    bool deny_parent = false) {
    const auto name = std::format("admin-{}-{}-{}-{}", su::deploy::target_id(target),
        face_result ? static_cast<int>(*face_result) : -1, password_accepts, deny_parent);
    const auto root = case_directory(name);
    const auto config_directory = root / "etc/pam.d";
    const auto socket_path = root / "control.sock";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(config_directory);
    {
        auto parent = std::ofstream(config_directory / su::deploy::target_service(target));
        parent << "auth required " << (deny_parent ? "pam_deny.so" : "pam_permit.so")
               << "\nauth include system-auth\naccount required pam_permit.so\n";
        auto password = std::ofstream(config_directory / "system-auth");
        // Stand-ins for a password verifier let us prove that the generated
        // stack reaches (or skips) the original fallback without host accounts.
        password << "auth required " << (password_accepts ? "pam_permit.so" : "pam_deny.so")
                 << '\n';
    }
    auto plan = su::deploy::plan_pam_integration(root, target, false);
    if (!plan) {
        return false;
    }
    constexpr auto module_name = std::string_view{"pam_smile2unlock.so"};
    const auto module_position = plan->child_content.find(module_name);
    if (module_position == std::string::npos) {
        return false;
    }
    // Exercise this build's module, even on hosts with an older installed copy.
    plan->child_content.replace(module_position, module_name.size(), SU_PAM_MODULE_PATH);
    const auto default_socket = plan->child_content.find(su::control::kDefaultSocketPath);
    if (default_socket == std::string::npos) {
        return false;
    }
    plan->child_content.replace(default_socket, su::control::kDefaultSocketPath.size(),
                                socket_path.string());
    if (!su::deploy::apply_pam_plan(root, *plan)) {
        return false;
    }
    auto pam = start_pam(config_directory, su::deploy::target_service(target));
    if (!pam) {
        return false;
    }
    const auto end_pam = PamGuard{*pam};
    auto status = PAM_AUTHINFO_UNAVAIL;
    if (!face_result) {
        status = ::pam_authenticate(*pam, 0);
    } else {
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
            if (request_id) {
                server_ok.store(connection->send_frame(su::control::make_response(
                    *request_id, *face_result, "administrator PAM test")).has_value());
            }
        });
        status = ::pam_authenticate(*pam, 0);
        server.join();
        if (!server_ok.load()) {
            return false;
        }
    }
    const auto accepted = !deny_parent
        && (face_result == su::control::ControlResult::kAccepted || password_accepts);
    const auto expected = accepted ? PAM_SUCCESS : PAM_AUTH_ERR;
    if (status != expected) {
        std::cerr << name << ": expected " << expected << ", got " << status << '\n';
    }
    return status == expected;
}

bool administrator_stacks_preserve_fallback() {
    for (const auto target : {su::deploy::TargetKind::kSudo,
             su::deploy::TargetKind::kSudoLogin, su::deploy::TargetKind::kPolkit}) {
        if (!run_administrator_case(target, su::control::ControlResult::kAccepted, false)
            || !run_administrator_case(target, su::control::ControlResult::kRejected, true)
            || !run_administrator_case(target, su::control::ControlResult::kRejected, false)
            || !run_administrator_case(target, std::nullopt, true)
            || !run_administrator_case(target, std::nullopt, false)
            || !run_administrator_case(target, su::control::ControlResult::kBusy, true)
            || !run_administrator_case(target, su::control::ControlResult::kAccepted, false, true)) {
            return false;
        }
    }
    return true;
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
            && run_socket_case(
                "malformed-response",
                su::control::ControlResult::kAccepted,
                PAM_AUTHINFO_UNAVAIL,
                true,
                true)
            && run_unavailable_case()
            && run_timeout_case("timeout", std::nullopt)
            && run_timeout_case("timeout-password-accepted", true)
            && run_timeout_case("timeout-password-rejected", false)
            && administrator_stacks_preserve_fallback()
        ? 0
        : 1;
}
