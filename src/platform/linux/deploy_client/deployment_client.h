#pragma once

#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace su::deploy {

struct AdoptionReview {
    std::string id;
    std::string description;
    // Holds the original bus connection: plans are bound to its unique sender.
    std::function<std::expected<std::string, std::string>()> apply;
};

class DeploymentClient {
public:
    [[nodiscard]] std::expected<std::string, std::string> collect_diagnostics() const;
    [[nodiscard]] bool dms_available() const;
    [[nodiscard]] bool installer_available() const;
    [[nodiscard]] std::expected<std::string, std::string> inspect() const;
    [[nodiscard]] std::expected<std::string, std::string> install_helper() const;
    [[nodiscard]] std::expected<std::string, std::string> initialize_runtime() const;
    [[nodiscard]] std::expected<std::string, std::string> authorize_profile_management(
        std::string_view operation) const;
    [[nodiscard]] std::expected<std::string, std::string> configure_target(
        std::string_view target,
        bool wallet_token) const;
    [[nodiscard]] std::expected<std::string, std::string>
    configure_target_source(std::string_view target, std::string_view authtok_source) const;
    [[nodiscard]] std::expected<AdoptionReview, std::string>
    plan_adoption(std::string_view target, std::string_view authtok_source = "none") const;
    [[nodiscard]] std::expected<std::string, std::string> rollback_target(
        std::string_view target) const;

private:
    // Managed PAM change plus the user-owned Noctalia config fragment.
    [[nodiscard]] std::expected<std::string, std::string> configure_noctalia() const;
    [[nodiscard]] std::expected<std::string, std::string> rollback_noctalia() const;
};

} // namespace su::deploy
