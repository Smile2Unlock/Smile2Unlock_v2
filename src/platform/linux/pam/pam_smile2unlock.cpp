#include "common/log/logger.h"
#include "common/log/pam_sink.h"
#include <security/pam_appl.h>
#include <security/pam_modules.h>
#include <security/pam_ext.h>
#include <unistd.h>

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

std::optional<su::control::ControlResponse> authenticate(
    std::string_view username, std::string_view socket_path, const su::log::Logger& logger) {
    const auto deadline = std::chrono::steady_clock::now() + kPamAuthenticationTimeout;
    auto connection = su::control::Connection::connect_to(socket_path, kPamAuthenticationTimeout);
    if (!connection) {
        logger.warn("daemon.connect_failed");
        return std::nullopt;
    }
    const auto request_id = next_request_id();
    const auto request_logger = logger.with({su::log::field("request_id", request_id)});
    const auto request = su::control::make_authenticate_request(request_id, username);
    if (!connection->send_frame(request, deadline)) {
        request_logger.warn("request.send_failed");
        return std::nullopt;
    }
    const auto response = connection->receive_frame(deadline);
    if (!response) {
        request_logger.warn("response.receive_failed");
        return std::nullopt;
    }
    const auto result = su::control::parse_response(*response);
    if (!result || result->request_id != request_id) { return std::nullopt; }
    return *result;
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
    const auto result = authenticate(username, socket_path_from_args(argc, argv), logger);
    const auto outcome = result ? pam_result(result->result) : PAM_AUTHINFO_UNAVAIL;
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
