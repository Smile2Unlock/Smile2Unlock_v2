#include "common/log/logger.h"
#include "common/log/pam_sink.h"
#include "common/security/sensitive.h"
#include "common/security/secret_json.h"
#include <security/pam_appl.h>
#include <security/pam_modules.h>
#include <security/pam_ext.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

import std;
import su.control.socket;

namespace {

// Leave time for the original password stack before a greeter's 10-second
// conversation timeout. This bounds the whole PAM exchange, including a
// stalled daemon; other control clients keep their existing frame timeouts.
constexpr auto kPamAuthenticationTimeout = std::chrono::seconds{8};

su::log::Logger pam_logger(pam_handle_t* handle) {
    static const auto level = [] {
        const auto setting = std::getenv("SMILE2UNLOCK_LOG_LEVEL");
        return setting ? su::log::parse_level(setting) : std::optional{su::log::Level::info};
    }();
    const auto logger = su::log::Logger{
        "pam", level.value_or(su::log::Level::info), su::log::pam_sink(handle)};
    static const auto warned = [&] {
        if (!level)
            logger.warn("log.level_invalid");
        return true;
    }();
    (void)warned;
    return logger;
}

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

bool sealed_authtok_from_args(int argc, const char** argv) {
    for (auto index = 0; index < argc; ++index) {
        if (std::string_view(argv[index]) == "authtok=sealed")
            return true;
    }
    return false;
}

void wipe(std::string& value) noexcept {
    if (!value.empty())
        ::explicit_bzero(value.data(), value.size());
    value.clear();
}

std::optional<std::vector<unsigned char>> decode_base64(std::string_view encoded) {
    constexpr auto alphabet =
        std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    if (encoded.empty() || encoded.size() > 2048 || encoded.size() % 4 != 0)
        return std::nullopt;
    auto decoded = std::vector<unsigned char>{};
    const auto wipe_decoded = su::security::ScopedWipe{decoded};
    decoded.reserve(encoded.size() / 4 * 3 + 1);
    for (auto offset = std::size_t{0}; offset < encoded.size(); offset += 4) {
        auto value = std::uint32_t{0};
        auto padding = 0;
        for (auto index = 0; index < 4; ++index) {
            const auto character = encoded[offset + static_cast<std::size_t>(index)];
            if (character == '=') {
                ++padding;
                value <<= 6;
            } else {
                if (padding != 0)
                    return std::nullopt;
                const auto digit = alphabet.find(character);
                if (digit == std::string_view::npos)
                    return std::nullopt;
                value = (value << 6) | static_cast<std::uint32_t>(digit);
            }
        }
        if (padding > 2 || (padding != 0 && offset + 4 != encoded.size()) ||
            (padding == 1 && (value & 0xff) != 0) || (padding == 2 && (value & 0xffff) != 0))
            return std::nullopt;
        decoded.push_back(static_cast<unsigned char>(value >> 16));
        if (padding < 2)
            decoded.push_back(static_cast<unsigned char>(value >> 8));
        if (padding < 1)
            decoded.push_back(static_cast<unsigned char>(value));
    }
    return std::optional{std::move(decoded)};
}

// This entire optional step is noexcept: a wallet error must never turn an
// already accepted face authentication into a PAM failure.
bool install_authtok(pam_handle_t* handle, std::string_view payload_json) noexcept {
    try {
        const void* current = nullptr;
        if (::pam_get_item(handle, PAM_AUTHTOK, &current) != PAM_SUCCESS)
            return false;
        if (current && *static_cast<const char*>(current))
            return true;
        auto payload = su::security::SecretJson::parse(payload_json, nullptr, false);
        const auto wipe_payload = su::security::ScopedJsonWipe{payload};
        if (!payload.is_object() || !payload.contains("authtok") || !payload["authtok"].is_string())
            return false;
        const auto& encoded = payload["authtok"].get_ref<const su::security::SecretString&>();
        auto token = decode_base64(std::string_view{encoded.data(), encoded.size()});
        if (!token)
            return false;
        const auto wipe_token = su::security::ScopedWipe{*token};
        if (token->empty() || token->size() > 1024 || std::ranges::find(*token, 0) != token->end())
            return false;
        token->push_back(0); // capacity reserved by decode_base64; no secret-bearing reallocation
        return ::pam_set_item(handle, PAM_AUTHTOK, reinterpret_cast<const char*>(token->data())) ==
               PAM_SUCCESS;
    } catch (...) {
        return false;
    }
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

bool chinese_messages(pam_handle_t* handle) {
    for (const auto name : {"LC_MESSAGES", "LANG"}) {
        const auto value = ::pam_getenv(handle, name);
        if (value && *value) { return std::string_view{value}.starts_with("zh"); }
    }
    for (const auto name : {"LC_MESSAGES", "LANG"}) {
        const auto value = std::getenv(name);
        if (value && *value) { return std::string_view{value}.starts_with("zh"); }
    }
    return false;
}

void show_info(pam_handle_t* handle, int flags, const char* text) {
    if ((flags & PAM_SILENT) == 0) { (void)::pam_info(handle, "%s", text); }
}

const char* failure_message(bool chinese, su::control::AuthenticationReason reason) {
    switch (reason) {
    case su::control::AuthenticationReason::kNoFace:
        return chinese ? "未检测到人脸，请正对摄像头并保持光线充足。"
            : "No face detected. Face the camera in good light.";
    case su::control::AuthenticationReason::kMismatch:
        return chinese ? "人脸不匹配，请使用密码。" : "Face not recognized. Use your password.";
    case su::control::AuthenticationReason::kUnknown:
        return chinese ? "人脸识别未完成，请使用密码。" : "Face recognition did not complete. Use your password.";
    }
    std::unreachable();
}

std::optional<su::control::ControlResponse> authenticate(std::string_view username,
                                                         std::string_view socket_path,
                                                         bool want_authtok,
                                                         const su::log::Logger& logger) {
    const auto deadline = std::chrono::steady_clock::now() + kPamAuthenticationTimeout;
    auto connection = su::control::Connection::connect_to(socket_path, kPamAuthenticationTimeout);
    if (!connection) {
        logger.warn("daemon.connect_failed");
        return std::nullopt;
    }
    const auto request_id = next_request_id();
    const auto request_logger = logger.with({su::log::field("request_id", request_id)});
    auto request = su::control::make_authenticate_request(request_id, username, want_authtok);
    if (!connection->send_frame(request, deadline)) {
        request_logger.warn("request.send_failed");
        return std::nullopt;
    }
    auto response = connection->receive_frame(deadline);
    if (!response) {
        request_logger.warn("response.receive_failed");
        return std::nullopt;
    }
    const auto wipe_frame = su::security::ScopedWipe{*response};
    auto result = su::control::parse_response(*response);
    wipe(*response);
    wipe(request);
    if (!result)
        return std::nullopt;
    const auto wipe_payload = su::security::ScopedWipe{result->payload_json};
    if (result->request_id != request_id)
        return std::nullopt;
    return std::move(*result);
}

} // namespace

PAM_EXTERN int pam_sm_authenticate(
    pam_handle_t* pamh,
    int flags,
    int argc,
    const char** argv) try {
    const auto logger = pam_logger(pamh);
    logger.info("authentication.entered");
    const char* username = nullptr;
    if (::pam_get_user(pamh, &username, nullptr) != PAM_SUCCESS
        || username == nullptr || username[0] == '\0') {
        logger.warn("authentication.user_unavailable");
        return PAM_USER_UNKNOWN;
    }
    const auto chinese = chinese_messages(pamh);
    show_info(pamh, flags, chinese ? "正在识别人脸，请正对摄像头"
        : "Recognizing your face, please look at the camera");
    auto result = authenticate(username, socket_path_from_args(argc, argv),
                               sealed_authtok_from_args(argc, argv), logger);
    const auto outcome = result ? pam_result(result->result) : PAM_AUTHINFO_UNAVAIL;
    if (result) {
        const auto wipe_payload = su::security::ScopedWipe{result->payload_json};
        if (sealed_authtok_from_args(argc, argv)) {
            const auto token_present =
                outcome == PAM_SUCCESS && install_authtok(pamh, result->payload_json);
            try {
                logger.info("authentication.authtok",
                            {su::log::field("token_present", token_present ? 1 : 0)});
            } catch (...) { /* optional-token logging cannot affect PAM status */
            }
        }
    }
    if (outcome != PAM_SUCCESS) {
        show_info(pamh, flags, failure_message(chinese, result
            ? result->authentication_reason : su::control::AuthenticationReason::kUnknown));
    }
    logger.info("authentication.completed", {su::log::field("pam_status", outcome)});
    return outcome;
} catch (...) {
    try { su::log::Logger{"pam", su::log::Level::info, su::log::pam_sink(pamh)}.error("authentication.exception"); } catch (...) {}
    // No C++ exception may cross the PAM C ABI or prevent password fallback.
    return PAM_AUTHINFO_UNAVAIL;
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
