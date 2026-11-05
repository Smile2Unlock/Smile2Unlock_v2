#include <security/pam_appl.h>
#include <unistd.h>

import std;
import su.control.socket;

namespace {
int conversation(int, const pam_message**, pam_response**, void*) { return PAM_CONV_ERR; }

bool wallet_case(std::string_view name, std::optional<su::control::ControlResult> face,
                 std::string_view payload, std::string_view expected, bool existing = false,
                 bool fallback_accepts = false, bool opt_in = true, bool slow = false) {
    const auto root = std::filesystem::absolute("build/test-data/pam") /
                      std::format("authtok-{}-{}", ::getpid(), name);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto socket =
        std::filesystem::path{"/tmp"} / std::format("su129-wallet-{}.sock", ::getpid());
    const auto dump = root / "wallet-token";
    {
        auto parent = std::ofstream(root / "wallet-test");
        if (existing)
            parent << "auth required " << SU_PAM_WALLET_MODULE_PATH << " seed=existing-token\n";
        parent << "auth substack face-test\nauth required " << SU_PAM_WALLET_MODULE_PATH
               << " output=" << dump.string() << '\n';
        auto child = std::ofstream(root / "face-test");
        child << "auth sufficient " << SU_PAM_MODULE_PATH << " socket=" << socket.string()
              << (opt_in ? " authtok=sealed" : "") << "\nauth substack password-test\n";
        auto password = std::ofstream(root / "password-test");
        if (fallback_accepts)
            password << "auth required " << SU_PAM_WALLET_MODULE_PATH << " seed=password-token\n";
        password << "auth required " << (fallback_accepts ? "pam_permit.so" : "pam_deny.so")
                 << '\n';
    }
    auto listener = face ? su::control::Listener::bind_to(socket.string())
                         : std::expected<su::control::Listener, su::control::SocketError>{
                               std::unexpected(su::control::SocketError::kInvalidArgument)};
    auto server_ok = std::atomic<bool>{!face};
    auto worker = std::jthread{};
    if (face) {
        if (!listener)
            return false;
        worker = std::jthread([&] {
            auto connection = listener->accept_one(std::chrono::seconds{10});
            if (!connection)
                return;
            const auto request = connection->receive_frame();
            if (!request)
                return;
            if (request->contains("\"want_authtok\":true") != opt_in)
                return;
            const auto marker = request->find("\"request_id\":");
            if (marker == std::string::npos)
                return;
            const auto digits = std::string_view(*request).substr(marker + 13);
            auto id = std::uint64_t{};
            if (std::from_chars(digits.data(), digits.data() + digits.size(), id).ec != std::errc{})
                return;
            if (slow)
                std::this_thread::sleep_for(std::chrono::seconds{3});
            server_ok =
                connection->send_frame(su::control::make_response(id, *face, "fixture", payload))
                    .has_value();
        });
    }
    const auto conv = pam_conv{.conv = conversation, .appdata_ptr = nullptr};
    pam_handle_t* handle = nullptr;
    const auto start =
        ::pam_start_confdir("wallet-test", "test-user", &conv, root.c_str(), &handle);
    const auto before = std::chrono::steady_clock::now();
    const auto status = start == PAM_SUCCESS ? ::pam_authenticate(handle, 0) : start;
    const auto elapsed = std::chrono::steady_clock::now() - before;
    if (handle)
        (void)::pam_end(handle, status);
    if (worker.joinable())
        worker.join();
    auto input = std::ifstream(dump);
    const auto token = std::string(std::istreambuf_iterator<char>(input), {});
    const auto success = face == su::control::ControlResult::kAccepted || fallback_accepts;
    const auto ok = server_ok && status == (success ? PAM_SUCCESS : PAM_AUTH_ERR) &&
                    token == expected && std::filesystem::exists(dump) &&
                    elapsed < std::chrono::seconds{8} &&
                    (!slow || elapsed >= std::chrono::seconds{3});
    if (!ok)
        std::cerr << "FAIL: PAM wallet " << name << " status=" << status
                  << " dump_exists=" << std::filesystem::exists(dump) << '\n';
    std::filesystem::remove_all(root);
    return ok;
}
} // namespace

bool pam_authtok_tests() {
    using R = su::control::ControlResult;
    constexpr auto token = R"({"authtok":"d2FsbGV0LXRva2Vu"})";
    return wallet_case("injected-auth", R::kAccepted, token, "wallet-token") &&
           wallet_case("preserve-existing", R::kAccepted, token, "existing-token", true) &&
           wallet_case("daemon-absent", std::nullopt, "null", "password-token", false, true) &&
           wallet_case("slow-3s", R::kAccepted, token, "wallet-token", false, false, true, true) &&
           wallet_case("not-enrolled", R::kAccepted, "null", "") &&
           wallet_case("old-daemon-no-payload", R::kAccepted, "{}", "") &&
           wallet_case("rejected", R::kRejected, token, "password-token", false, true) &&
           wallet_case("rejected-password-fails", R::kRejected, token, "") &&
           wallet_case("invalid-base64", R::kAccepted, R"({"authtok":"!!!!"})", "") &&
           wallet_case("wrong-type", R::kAccepted, R"({"authtok":42})", "") &&
           wallet_case("embedded-nul", R::kAccepted, R"({"authtok":"YQBi"})", "") &&
           wallet_case("default-off", R::kAccepted, token, "", false, false, false);
}
