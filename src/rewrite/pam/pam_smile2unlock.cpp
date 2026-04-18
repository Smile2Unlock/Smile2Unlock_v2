#include <security/pam_appl.h>
#include <security/pam_modules.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstdio>

#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>
#include <array>
#include <optional>

#include "rewrite/backend_core.hpp"

import rewrite.protocol;
import rewrite.runtime;
import rewrite.socket;

namespace {

using namespace rewrite::ipc;
using namespace rewrite::net;
using namespace rewrite::runtime;

constexpr std::size_t kAuthTimeoutSeconds = 30;

struct PamConvData {
    int retcode;
    std::string message;
};

int pam_conv_func(int num_msg, const struct pam_message** msg, struct pam_response** resp, void* appdata_ptr) {
    (void)num_msg;
    (void)msg;
    (void)resp;
    (void)appdata_ptr;
    return PAM_SUCCESS;
}

class PamAuthClient {
public:
    PamAuthClient() = default;

    ~PamAuthClient() {
        disconnect();
    }

    [[nodiscard]] std::expected<void, std::string> connect_to_backend() {
        const auto eps = default_endpoints();

        SocketClient client;
        if (auto r = client.connect(ListenEndpoint{{eps.control}}); !r) {
            return std::unexpected(r.error());
        }
        connection_ = SocketConnection(client.native_handle());
        return {};
    }

    void disconnect() {
        connection_.close();
    }

    [[nodiscard]] std::expected<void, std::string> authenticate(const std::string& username) {
        if (!connection_) {
            return std::unexpected("not connected");
        }

        FrameHeaderV1 header{};
        header.msg_type = static_cast<std::uint16_t>(MessageType::control_request);
        header.flags = flag_value(Flag::request);
        header.request_id = next_request_id_++;

        std::string body = std::format(
            R"({{"command":"AUTH_BEGIN","username":"{}","client":"pam"}})",
            username);

        header.body_bytes = static_cast<std::uint32_t>(body.size());
        header.body_codec = static_cast<std::uint16_t>(BodyCodec::utf8_json);

        if (auto r = connection_.send_header(header); !r) {
            return std::unexpected(r.error());
        }
        if (auto r = connection_.send_bytes(std::as_bytes(std::span(body))); !r) {
            return std::unexpected(r.error());
        }

        return receive_auth_response();
    }

private:
    [[nodiscard]] std::expected<void, std::string> receive_auth_response() {
        auto header_bytes = connection_.receive_bytes(sizeof(FrameHeaderV1));
        if (!header_bytes) {
            return std::unexpected("failed to receive header");
        }

        auto header = parse_header(std::as_bytes(std::span(*header_bytes)));
        if (!header) {
            return std::unexpected(header.error());
        }

        if (header->msg_type != static_cast<std::uint16_t>(MessageType::control_response)) {
            return std::unexpected("unexpected message type");
        }

        std::vector<std::byte> body;
        if (header->body_bytes > 0) {
            auto body_result = connection_.receive_all(header->body_bytes);
            if (!body_result) {
                return std::unexpected("failed to receive body");
            }
            body = std::move(*body_result);
        }

        std::string body_str(reinterpret_cast<const char*>(body.data()), body.size());
        if (body_str.find("\"accepted\":true") != std::string::npos ||
            body_str.find("\"status\":\"ok\"") != std::string::npos) {
            return {};
        }

        return std::unexpected(std::format("auth rejected: {}", body_str));
    }

    SocketConnection connection_;
    std::uint64_t next_request_id_{1};
};

} // namespace

extern "C" {

PAM_EXTERN int pam_sm_authenticate(pam_handle_t* pamh, int flags, int argc, const char** argv) {
    (void)flags;
    (void)argc;
    (void)argv;

    if (!pamh) {
        return PAM_AUTH_ERR;
    }

    const char* username_cstr = nullptr;
    if (pam_get_user(pamh, &username_cstr, "Username: ") != PAM_SUCCESS) {
        return PAM_AUTH_ERR;
    }

    if (!username_cstr || !*username_cstr) {
        return PAM_AUTH_ERR;
    }

    std::string username(username_cstr);

    struct passwd* pw = getpwnam(username.c_str());
    if (!pw) {
        return PAM_USER_UNKNOWN;
    }

    if (getuid() == 0 || geteuid() == 0) {
        return PAM_IGNORE;
    }

    PamAuthClient client;
    if (auto r = client.connect_to_backend(); !r) {
        fprintf(stderr, "[pam_smile2unlock] failed to connect to backend: %s\n", r.error().c_str());
        return PAM_AUTHINFO_UNAVAIL;
    }

    if (auto r = client.authenticate(username); !r) {
        fprintf(stderr, "[pam_smile2unlock] authentication failed: %s\n", r.error().c_str());
        return PAM_AUTH_ERR;
    }

    fprintf(stderr, "[pam_smile2unlock] user %s authenticated successfully\n", username.c_str());
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t* pamh, int flags, int argc, const char** argv) {
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t* pamh, int flags, int argc, const char** argv) {
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_open_session(pam_handle_t* pamh, int flags, int argc, const char** argv) {
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_close_session(pam_handle_t* pamh, int flags, int argc, const char** argv) {
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_chauthtok(pam_handle_t* pamh, int flags, int argc, const char** argv) {
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}

} // extern "C"