#include "app/wallet_settings.h"
#include "platform/linux/deploy/deployment.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

import su.control.socket;

namespace {
auto failures = 0;
void check(bool ok, std::string_view name) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}

// Verify the buffers while they are still alive, after the workflow's wipe
// guards have run. Looking at freed allocations would be undefined behavior.
struct CheckedBuffer {
    std::vector<char> bytes;
    explicit CheckedBuffer(std::string_view text) : bytes(text.begin(), text.end()) {}
    CheckedBuffer(CheckedBuffer&&) = default;
    ~CheckedBuffer() {
        if (!bytes.empty())
            check(std::ranges::all_of(bytes, [](char byte) { return byte == 0; }),
                  "capability/wire buffer wiped before destruction");
    }
    char* data() { return bytes.data(); }
    std::size_t size() const { return bytes.size(); }
    operator std::string_view() const { return {bytes.data(), bytes.size()}; }
};
} // namespace

int main() {
    namespace wallet = su::app::wallet;
    using su::deploy::AuthtokSource;
    for (const auto source :
         {AuthtokSource::kNone, AuthtokSource::kKernelKeyring, AuthtokSource::kSealed}) {
        const auto planner_id = su::deploy::authtok_source_id(source);
        check(wallet::source_id(wallet::source_index(planner_id)) == planner_id,
              "UI/planner source round trip");
        check(su::deploy::authtok_source_from_id(
                  wallet::source_id(wallet::source_index(planner_id))) == source,
              "UI source maps to planner enum");
    }
    check(wallet::source_id(0) == "none", "default is off");
    check(wallet::source_id(-1) == "none" && wallet::source_id(3) == "none",
          "invalid selection stays off");
    check(wallet::sealed_unavailable_reason(true, true, true).empty(), "eligible sealed target");
    check(wallet::sealed_unavailable_reason(false, true, true) ==
              "deployment.wallet_target_unavailable",
          "target reason");
    check(wallet::sealed_unavailable_reason(true, false, true) ==
              "deployment.sealed_parent_required",
          "parent reason");
    check(wallet::sealed_unavailable_reason(true, true, false) == "wallet.tpm_required",
          "TPM reason");
    check(wallet::use_unavailable_reason(true, true, true, true, true).empty(), "eligible user");
    check(wallet::use_unavailable_reason(false, true, true, true, true).empty(),
          "service message is owned by management availability");
    check(wallet::use_unavailable_reason(true, false, true, true, true).empty(),
          "unknown status message appears only in status line");
    check(wallet::use_unavailable_reason(true, true, false, true, true) == "wallet.face_required",
          "face reason");
    check(wallet::use_unavailable_reason(true, true, true, false, true) == "wallet.tpm_required",
          "user TPM reason");
    check(wallet::use_unavailable_reason(true, true, true, true, false) == "wallet.login_required",
          "login source reason");

    // set/update and clear; authorization refusal, send failure, exception.
    for (const auto clear : {false, true}) {
        for (auto failure = 0; failure < 5; ++failure) {
            auto password = wallet::secret_buffer();
            password->assign(clear ? "" : "short\"\\\n密码");
            auto authorized = 0;
            auto built = 0;
            auto sent = 0;
            auto threw = false;
            auto accepted = false;
            try {
                const auto result = wallet::manage_authtok(
                    clear, *password,
                    [&](std::string_view operation) -> std::expected<CheckedBuffer, std::string> {
                        ++authorized;
                        check(operation == (clear ? "clear_authtok" : "set_authtok"),
                              "matching capability operation");
                        if (failure == 1)
                            return std::unexpected("denied");
                        return CheckedBuffer{"one-use-capability"};
                    },
                    [&](bool remove, std::string_view secret, std::string_view token) {
                        ++built;
                        if (failure == 4)
                            throw std::runtime_error{"construction failed"};
                        auto request =
                            remove
                                ? su::control::make_clear_authtok_request(42, "fixture-user", token)
                                : su::control::make_set_authtok_request(42, "fixture-user", secret,
                                                                        token);
                        const auto wipe = su::security::ScopedWipe{request};
                        return CheckedBuffer{request};
                    },
                    [&](std::string_view request) -> std::expected<void, std::string> {
                        ++sent;
                        const auto json = nlohmann::json::parse(request);
                        check(json.at("msg_type") == (clear ? "clear_authtok" : "set_authtok"),
                              "request type");
                        check(json.at("username") == "fixture-user" && json.at("request_id") == 42,
                              "request identity");
                        check(json.at("management_token") == "one-use-capability",
                              "request capability");
                        check(clear ? !json.contains("authtok") : json.at("authtok") == *password,
                              "only set includes escaped secret");
                        if (failure == 3)
                            throw std::runtime_error{"send failed"};
                        if (failure == 2)
                            return std::unexpected("unavailable");
                        return {};
                    });
                accepted = result.has_value();
            } catch (const std::runtime_error&) {
                threw = true;
            }
            check(authorized == 1 && built == (failure == 1 ? 0 : 1) &&
                      sent == ((failure == 1 || failure == 4) ? 0 : 1),
                  "authorize precedes construction and send");
            check(accepted == (failure == 0) && threw == (failure >= 3), "result propagation");
            check(std::ranges::all_of(*password, [](char byte) { return byte == 0; }),
                  "secret not retained on any exit");
        }
    }
    for (const auto& password : {std::string{}, std::string(1025, 'x'), std::string("a\0b", 3)}) {
        auto input = password;
        const auto result = wallet::manage_authtok(
            false, input,
            [](std::string_view) -> std::expected<std::string, std::string> {
                check(false, "invalid password must not request capability");
                return "";
            },
            [](bool, std::string_view, std::string_view) { return std::string{}; },
            [](std::string_view) -> std::expected<void, std::string> { return {}; });
        check(!result && std::ranges::all_of(input, [](char byte) { return byte == 0; }),
              "invalid secret rejected and wiped");
    }
    if (!failures)
        std::cout << "wallet settings: source mapping, eligibility, capability ordering and "
                     "all-path wiping passed\n";
    return failures ? 1 : 0;
}
