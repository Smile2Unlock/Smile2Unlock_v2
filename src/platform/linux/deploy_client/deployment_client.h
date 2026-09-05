#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace su::deploy {

class DeploymentClient {
public:
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
    [[nodiscard]] std::expected<std::string, std::string> rollback_target(
        std::string_view target) const;
};

} // namespace su::deploy
