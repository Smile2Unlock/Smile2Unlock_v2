#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace su::windows::profile_client {

std::expected<std::string, std::string> list_profiles();
std::expected<void, std::string> store_account_credential(
    std::string_view windows_password);
std::expected<bool, std::string> account_credential_configured();
std::expected<std::string, std::string> enroll_profile(
    std::string_view label,
    std::string_view embedding_source);
std::expected<bool, std::string> delete_profile(std::string_view profile_id);
std::expected<std::string, std::string> verify_profile(
    std::string_view embedding_source,
    bool liveness_ok);

} // namespace su::windows::profile_client
