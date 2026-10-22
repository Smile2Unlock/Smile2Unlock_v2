#include <security/pam_appl.h>
#include <stdlib.h>
#include <unistd.h>

import std;
import su.control.socket;

namespace {

struct Case {
    std::string_view locale;
    su::control::AuthenticationReason reason;
    std::string_view expected;
    int flags = 0;
    su::control::ControlResult result = su::control::ControlResult::kRejected;
    const char* locale_variable = "LANG";
    bool process_locale = false;
};

class LocaleGuard {
public:
    LocaleGuard() : lang_(read("LANG")), messages_(read("LC_MESSAGES")) {
        (void)::setenv("LANG", "C", 1);
        (void)::unsetenv("LC_MESSAGES");
    }
    ~LocaleGuard() { restore("LANG", lang_); restore("LC_MESSAGES", messages_); }
private:
    static std::optional<std::string> read(const char* name) {
        const auto value = std::getenv(name);
        return value ? std::optional{std::string{value}} : std::nullopt;
    }
    static void restore(const char* name, const std::optional<std::string>& value) {
        if (value) { (void)::setenv(name, value->c_str(), 1); }
        else { (void)::unsetenv(name); }
    }
    std::optional<std::string> lang_;
    std::optional<std::string> messages_;
};

void configure_locale(pam_handle_t* handle, const Case& value) {
    if (value.process_locale) {
        (void)::setenv(value.locale_variable, std::string{value.locale}.c_str(), 1);
    } else {
        (void)::pam_putenv(handle, "LANG=en_US.UTF-8");
        const auto setting = std::string{value.locale_variable} + "=" + std::string{value.locale};
        (void)::pam_putenv(handle, setting.c_str());
    }
}

int conversation(int count, const pam_message** input, pam_response** output, void* data) {
    auto& messages = *static_cast<std::vector<std::string>*>(data);
    auto* responses = static_cast<pam_response*>(std::calloc(count, sizeof(pam_response)));
    if (!responses) { return PAM_BUF_ERR; }
    for (int i = 0; i < count; ++i) {
        if (input[i]->msg_style != PAM_TEXT_INFO) {
            std::free(responses);
            return PAM_CONV_ERR;
        }
        messages.emplace_back(input[i]->msg);
    }
    *output = responses;
    return PAM_SUCCESS;
}

bool serve(su::control::Listener& listener, const Case& value) {
    auto connection = listener.accept_one(std::chrono::seconds{2});
    if (!connection) { return false; }
    const auto request = connection->receive_frame();
    if (!request) { return false; }
    constexpr std::string_view key = R"("request_id":)";
    const auto start = request->find(key);
    if (start == std::string::npos) { return false; }
    const auto digits = std::string_view{*request}.substr(start + key.size());
    std::uint64_t id = 0;
    if (std::from_chars(digits.data(), digits.data() + digits.size(), id).ec != std::errc{}) {
        return false;
    }
    return connection->send_frame(su::control::make_response(
        id, value.result, "diagnostic text must never reach PAM", "null", value.reason)).has_value();
}

bool run_case(const Case& value, int index) {
    const auto root = std::filesystem::path{"build/test-data/pam"}
        / std::format("guidance-{}-{}", ::getpid(), index);
    std::filesystem::create_directories(root);
    const auto socket = root / "control.sock";
    { auto config = std::ofstream(root / "guidance");
      config << "auth required " << SU_PAM_MODULE_PATH << " socket=" << socket.string() << '\n'; }
    auto listener = su::control::Listener::bind_to(socket.string());
    if (!listener) { return false; }
    std::vector<std::string> messages;
    const auto conv = pam_conv{conversation, &messages};
    pam_handle_t* handle = nullptr;
    if (::pam_start_confdir("guidance", "test-user", &conv, root.c_str(), &handle) != PAM_SUCCESS) {
        return false;
    }
    const auto locale_guard = LocaleGuard{};
    configure_locale(handle, value);
    std::atomic<bool> server_ok = false;
    auto server = std::jthread([&] { server_ok = serve(*listener, value); });
    const auto status = ::pam_authenticate(handle, value.flags);
    (void)::pam_end(handle, status);
    server.join();
    const auto expected_status = value.result == su::control::ControlResult::kAccepted
        ? PAM_SUCCESS : PAM_AUTH_ERR;
    const auto silent = (value.flags & PAM_SILENT) != 0;
    const auto chinese = value.locale.starts_with("zh");
    const auto initial = chinese ? "正在识别人脸，请正对摄像头"
        : "Recognizing your face, please look at the camera";
    const auto expected_count = silent ? 0u : value.result == su::control::ControlResult::kAccepted ? 1u : 2u;
    const auto passed = status == expected_status && server_ok && messages.size() == expected_count
        && (silent || (messages.front() == initial
            && (expected_count == 1 || messages.back() == value.expected)));
    if (!passed) { std::cerr << "PAM guidance case " << index << " failed\n"; }
    std::filesystem::remove_all(root);
    return passed;
}

bool response_compatibility() {
    using enum su::control::AuthenticationReason;
    const auto old = su::control::parse_response(su::control::make_response(
        1, su::control::ControlResult::kRejected, "old diagnostic"));
    const auto future = su::control::parse_response(R"({"version":2,"msg_type":"auth_result","request_id":1,"result":"rejected","reason":"","authentication_reason":"future"})");
    const auto invalid = su::control::parse_response(R"({"version":2,"msg_type":"auth_result","request_id":1,"result":"rejected","reason":"","authentication_reason":8})");
    return old && old->authentication_reason == kUnknown
        && future && future->authentication_reason == kUnknown && !invalid;
}

} // namespace

bool pam_guidance_tests() {
    using enum su::control::AuthenticationReason;
    const auto cases = std::array{
        Case{"en_US.UTF-8", kNoFace, "No face detected. Face the camera in good light."},
        Case{"zh_CN.UTF-8", kNoFace, "未检测到人脸，请正对摄像头并保持光线充足。"},
        Case{"en_US.UTF-8", kMismatch, "Face not recognized. Use your password."},
        Case{"zh_TW.UTF-8", kMismatch, "人脸不匹配，请使用密码。"},
        Case{"C", kUnknown, "Face recognition did not complete. Use your password."},
        Case{"zh_CN.UTF-8", kUnknown, "人脸识别未完成，请使用密码。"},
        Case{"zh_CN.UTF-8", kNoFace, "", PAM_SILENT},
        Case{"en_US.UTF-8", kMismatch, "", PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK},
        Case{"zh_CN.UTF-8", kUnknown, "", 0, su::control::ControlResult::kAccepted},
        Case{"en_US.UTF-8", kUnknown, "", PAM_SILENT, su::control::ControlResult::kAccepted},
        Case{"zh_CN.UTF-8", kNoFace, "未检测到人脸，请正对摄像头并保持光线充足。", 0,
            su::control::ControlResult::kRejected, "LC_MESSAGES"},
        Case{"zh_CN.UTF-8", kNoFace, "未检测到人脸，请正对摄像头并保持光线充足。", 0,
            su::control::ControlResult::kRejected, "LC_MESSAGES", true},
        Case{"zh_CN.UTF-8", kMismatch, "人脸不匹配，请使用密码。", 0,
            su::control::ControlResult::kRejected, "LANG", true},
        Case{"", kUnknown, "Face recognition did not complete. Use your password.", 0,
            su::control::ControlResult::kRejected, "LANG", true},
    };
    auto index = 0;
    return response_compatibility() && std::ranges::all_of(cases, [&](const auto& value) {
        return run_case(value, index++);
    });
}
