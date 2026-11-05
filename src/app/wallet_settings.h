#pragma once

#include "common/security/sensitive.h"
#include <array>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace su::app::wallet {

inline constexpr auto sources = std::array<std::string_view, 3>{"none", "kernel_keyring", "sealed"};
inline int source_index(std::string_view source) {
    for (auto index = 0; index < static_cast<int>(sources.size()); ++index)
        if (sources[index] == source)
            return index;
    return 0;
}
inline std::string_view source_id(int index) {
    return index >= 0 && index < static_cast<int>(sources.size()) ? sources[index] : "none";
}

// These are translation keys, never planner errors or untranslated UI text.
inline std::string_view sealed_unavailable_reason(bool configurable, bool parent_wallet_auth,
                                                  bool tpm_protected) {
    if (!configurable)
        return "deployment.wallet_target_unavailable";
    if (!parent_wallet_auth)
        return "deployment.sealed_parent_required";
    if (!tpm_protected)
        return "wallet.tpm_required";
    return {};
}

inline std::string_view use_unavailable_reason(bool service, bool status_known, bool has_profile,
                                               bool tpm_protected, bool sealed_login) {
    if (!service || !status_known)
        return "wallet.status_unavailable";
    if (!has_profile)
        return "wallet.face_required";
    if (!tpm_protected)
        return "wallet.tpm_required";
    if (!sealed_login)
        return "wallet.login_required";
    return {};
}

// A shared owner crosses the worker boundary without copying secret bytes.
// Its deleter also covers allocation/worker-launch exceptions and early exits.
inline std::shared_ptr<std::string> secret_buffer() {
    return {new std::string{}, [](std::string* secret) {
                su::security::wipe_bytes(secret->data(), secret->size());
                delete secret;
            }};
}

// The caller owns secret for the duration of the call. Every exit (including
// authorization failure and exceptions) wipes it, the capability and wire JSON.
// Injected boundaries let tests exercise exactly the controller's transaction.
template <typename Authorize, typename MakeRequest, typename Send>
std::expected<void, std::string> manage_authtok(bool clear, std::string& secret,
                                                Authorize authorize, MakeRequest make_request,
                                                Send send) {
    const auto wipe_secret = su::security::ScopedWipe{secret};
    if (!clear &&
        (secret.empty() || secret.size() > 1024 || secret.find('\0') != std::string::npos))
        return std::unexpected("invalid wallet password");
    const auto operation = clear ? std::string_view{"clear_authtok"} : "set_authtok";
    auto capability = authorize(operation);
    if (!capability)
        return std::unexpected(capability.error());
    const auto wipe_capability = su::security::ScopedWipe{*capability};
    auto request = make_request(clear, std::string_view{secret}, std::string_view{*capability});
    const auto wipe_request = su::security::ScopedWipe{request};
    return send(std::string_view{request});
}

} // namespace su::app::wallet
