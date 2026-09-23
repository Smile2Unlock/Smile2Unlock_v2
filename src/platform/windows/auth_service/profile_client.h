#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace su::windows::profile_client {

/// Trigger policy pushed to the LocalSystem service, which persists it
/// machine-wide so the credential provider can read it at cold boot.
/// Field order and widths must match logon_secret_ipc::RecognitionSettingsPayload.
struct RecognitionSettings {
    std::uint32_t camera_index = 0;
    std::uint32_t recognition_threshold_milli = 650;
    std::uint32_t liveness_enabled = 1;
    std::uint32_t liveness_threshold_milli = 500;
    std::uint32_t recognition_mode = 0;
    std::uint32_t auto_delay_sec = 3;
    std::uint32_t retry_delay_sec = 5;
    std::uint32_t timeout_sec = 30;
};

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
std::expected<void, std::string> store_recognition_settings(
    const RecognitionSettings& settings);

} // namespace su::windows::profile_client
