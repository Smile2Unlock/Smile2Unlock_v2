#include <security/pam_appl.h>
#include <security/pam_modules.h>
#include <unistd.h>

import std;
import su.control.socket;

namespace {

std::string_view socket_path_from_args(int argc, const char** argv) {
    constexpr auto prefix = std::string_view{"socket="};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view(argv[index]);
        if (argument.starts_with(prefix) && argument.size() > prefix.size()) {
            return argument.substr(prefix.size());
        }
    }
    return su::control::kDefaultSocketPath;
}

std::uint64_t next_request_id() {
    static auto sequence = std::atomic<std::uint64_t>{1};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return static_cast<std::uint64_t>(now)
        ^ (static_cast<std::uint64_t>(::getpid()) << 32)
        ^ sequence.fetch_add(1, std::memory_order_relaxed);
}

int pam_result(su::control::ControlResult result) {
    switch (result) {
    case su::control::ControlResult::kAccepted:
        return PAM_SUCCESS;
    case su::control::ControlResult::kRejected:
    case su::control::ControlResult::kCancelled:
        return PAM_AUTH_ERR;
    case su::control::ControlResult::kUnavailable:
    case su::control::ControlResult::kBusy:
    case su::control::ControlResult::kError:
        return PAM_AUTHINFO_UNAVAIL;
    }
    std::unreachable();
}

} // namespace

PAM_EXTERN int pam_sm_authenticate(
    pam_handle_t* pamh,
    int flags,
    int argc,
    const char** argv) {
    (void)flags;

    const char* username = nullptr;
    if (::pam_get_user(pamh, &username, nullptr) != PAM_SUCCESS
        || username == nullptr
        || username[0] == '\0') {
        return PAM_USER_UNKNOWN;
    }

    auto connection = su::control::Connection::connect_to(
        socket_path_from_args(argc, argv));
    if (!connection) {
        return PAM_AUTHINFO_UNAVAIL;
    }

    const auto request = su::control::make_authenticate_request(
        next_request_id(),
        username);
    if (auto sent = connection->send_frame(request); !sent) {
        return PAM_AUTHINFO_UNAVAIL;
    }
    const auto response = connection->receive_frame();
    if (!response) {
        return PAM_AUTHINFO_UNAVAIL;
    }
    const auto result = su::control::parse_response(*response);
    return result ? pam_result(*result) : PAM_AUTHINFO_UNAVAIL;
}

PAM_EXTERN int pam_sm_setcred(
    pam_handle_t* pamh,
    int flags,
    int argc,
    const char** argv) {
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}
