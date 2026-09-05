export module su.core.types;

import std;

export namespace su::app {

enum class CoreError {
    kNullArgument,
    kInvalidUtf8,
    kUserDenied,
    kIoError,
    kParseError,
    kWriteError,
    kInvalidArgument,
    kBufferTooSmall,
    kCryptoError,
    kKeyUnavailable,
    kMigrationRequired,
    kUnknown,
};

struct EncryptedStoreContext {
    std::span<const std::uint8_t, 32> master_key;
    std::uint32_t key_version = 1;
    std::variant<std::uint32_t, std::string> account;
};

struct CoreConfig {
    std::uint32_t version = 1;
    int selected_camera = 0;
    float recognition_threshold = 0.65F;
    bool liveness_detection = true;
    float liveness_threshold = 0.50F;
    std::uint32_t preview_fps = 15;
    // Recognition trigger policy (0 = manual, 1 = auto). Mirrors
    // HKLM\SOFTWARE\Smile2Unlock\Recognition on Windows.
    std::uint32_t recognition_mode = 0;
    std::uint32_t auto_delay_sec = 3;
    std::uint32_t retry_delay_sec = 5;
    std::uint32_t timeout_sec = 30;
};

struct AuthDecision {
    bool accepted = false;
};

enum class ControlMessageType {
    kAuthenticate,
    kStatus,
    kCancel,
    kStorageStatus,
    kListProfiles,
    kEnrollProfile,
    kDeleteProfile,
    kMigrateProfiles,
    kVerifyProfile,
    kIssueManagementCapability,
};

struct ControlRequest {
    ControlMessageType type = ControlMessageType::kStatus;
    std::uint64_t request_id = 0;
    std::uint64_t target_request_id = 0;
    std::uint32_t target_uid = 0;
    std::uint32_t target_pid = 0;
    std::string username;
    std::string profile_id;
    std::string label;
    std::string face_sample_source;
    std::string management_operation;
    std::string management_token;
    bool liveness_ok = false;
};

struct FaceAuthDecision {
    bool accepted = false;
    float score = 0.0F;
    std::uint32_t profile_count = 0;
};

struct FaceProfileSummary {
    std::string id;
    std::string label;
    std::uint64_t created_at_unix = 0;
};

struct FaceAuthReport {
    bool accepted = false;
    float score = 0.0F;
    float threshold = 0.0F;
    bool liveness_ok = true;
    std::uint32_t profile_count = 0;
    std::string best_profile_id;
    std::string best_profile_label;
    std::string reason;
};

std::uint32_t core_version_major();
std::expected<ControlRequest, CoreError> parse_control_request(std::string_view json);
std::expected<float, CoreError> default_threshold();
CoreConfig default_config();
std::expected<CoreConfig, CoreError> load_config(const std::string& path);
std::expected<void, CoreError> save_config(const std::string& path, const CoreConfig& config);
std::expected<AuthDecision, CoreError> evaluate_auth(
    std::string_view username,
    float similarity,
    float threshold,
    bool liveness_ok);
std::expected<void, CoreError> enroll_face_profile(
    const std::string& store_path,
    std::string_view label,
    std::string_view face_sample_source);
std::expected<bool, CoreError> delete_face_profile(
    const std::string& store_path,
    std::string_view profile_id);
std::expected<std::string, CoreError> list_face_profiles_json(const std::string& store_path);
std::expected<std::vector<FaceProfileSummary>, CoreError> list_face_profile_summaries(
    const std::string& store_path);
std::expected<FaceAuthDecision, CoreError> authenticate_face_sample(
    const std::string& store_path,
    std::string_view face_sample_source,
    float threshold);
std::expected<FaceAuthDecision, CoreError> authenticate_face_sample(
    const std::string& store_path,
    std::string_view face_sample_source,
    float threshold,
    bool liveness_ok);
std::expected<std::string, CoreError> authenticate_face_sample_report_json(
    const std::string& store_path,
    std::string_view face_sample_source,
    float threshold);
std::expected<std::string, CoreError> authenticate_face_sample_report_json(
    const std::string& store_path,
    std::string_view face_sample_source,
    float threshold,
    bool liveness_ok);
std::expected<FaceAuthReport, CoreError> authenticate_face_sample_report(
    const std::string& store_path,
    std::string_view face_sample_source,
    float threshold);
std::expected<FaceAuthReport, CoreError> authenticate_face_sample_report(
    const std::string& store_path,
    std::string_view face_sample_source,
    float threshold,
    bool liveness_ok);
std::expected<void, CoreError> enroll_encrypted_face_profile(
    const EncryptedStoreContext& context,
    const std::string& store_path,
    std::string_view label,
    std::string_view face_sample_source);
std::expected<bool, CoreError> delete_encrypted_face_profile(
    const EncryptedStoreContext& context,
    const std::string& store_path,
    std::string_view profile_id);
std::expected<std::string, CoreError> list_encrypted_face_profiles_json(
    const EncryptedStoreContext& context,
    const std::string& store_path);
std::expected<std::vector<FaceProfileSummary>, CoreError> list_encrypted_face_profile_summaries(
    const EncryptedStoreContext& context,
    const std::string& store_path);
std::expected<FaceAuthReport, CoreError> authenticate_encrypted_face_sample_report(
    const EncryptedStoreContext& context,
    const std::string& store_path,
    std::string_view face_sample_source,
    float threshold,
    bool liveness_ok);
std::expected<bool, CoreError> migrate_plaintext_face_profiles(
    const EncryptedStoreContext& context,
    const std::string& legacy_path,
    const std::string& encrypted_path);

} // namespace su::app
