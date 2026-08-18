export module su.app.controller;

import std;
import su.recognizer.types;
import su.recognizer.service;
import su.core.types;

export namespace su::app {

struct AppSnapshot {
    std::string title;
    std::vector<su::recognizer::CameraInfo> cameras;
    CoreConfig config;
    std::string config_path;
    std::string profile_store_path;
    std::vector<FaceProfileSummary> profiles;
    std::string profiles_json;
    bool slint_enabled = false;
    bool seetaface_available = false;
};

struct FaceDemoSnapshot {
    std::vector<FaceProfileSummary> profiles;
    std::string profiles_json;
    std::string auth_report_json;
    FaceAuthDecision decision;
    FaceAuthReport report;
};

enum class StorageProtection {
    kUnavailable,
    kHostKey,
    kHostTpm2,
    kUnknown,
};

struct DeploymentTargetStatus {
    std::string id;
    std::string service;
    std::string effective_path;
    std::string role;
    std::string state;
    std::string detail;
    bool password_fallback = false;
    bool configured = false;
    bool configurable = false;
    bool managed = false;
    bool wallet_available = false;
    bool wallet_enabled = false;
};

struct SystemStatus {
    bool service_available = false;
    bool account_credential_configured = false;
    std::string service_reason;
    StorageProtection storage_protection = StorageProtection::kUnavailable;
    bool pam_status_known = false;
    bool pam_configured = false;
    std::string pam_service;
    bool deployment_helper_available = false;
    bool deployment_installer_available = false;
    bool login_pam_configured = false;
    bool lock_pam_configured = false;
    std::vector<DeploymentTargetStatus> deployment_targets;
};

class AppController {
public:
    std::expected<AppSnapshot, std::string> load_initial_snapshot();
    std::vector<su::recognizer::CameraInfo> enumerate_cameras() const;
    std::expected<bool, std::string> evaluate_demo_auth(std::string_view username);
    std::expected<CoreConfig, std::string> load_config_snapshot();
    std::expected<void, std::string> save_config_snapshot(const CoreConfig& config);
    std::expected<FaceDemoSnapshot, std::string> run_face_demo(
        std::string_view label,
        std::string_view enroll_sample_source,
        std::string_view probe_sample_source);
    std::expected<std::string, std::string> enroll_face_profile_from_sample(
        std::string_view label,
        std::string_view face_sample_source);
    std::expected<std::string, std::string> enroll_face_profile_from_current_frame(
        std::string_view label);
    std::expected<FaceDemoSnapshot, std::string> authenticate_face_sample_from_source(
        std::string_view face_sample_source);
    std::expected<FaceDemoSnapshot, std::string> authenticate_face_sample_from_source(
        std::string_view face_sample_source,
        bool liveness_ok);
    std::expected<FaceDemoSnapshot, std::string> authenticate_current_frame();
    std::expected<std::string, std::string> list_face_profiles();
    std::expected<std::vector<FaceProfileSummary>, std::string> list_face_profile_rows();
    std::expected<bool, std::string> delete_face_profile_by_id(std::string_view profile_id);
    std::expected<void, std::string> configure_windows_account_credential(
        std::string_view windows_password);
    SystemStatus load_system_status();
    std::expected<std::string, std::string> install_deployment_helper();
    std::expected<std::string, std::string> initialize_system_deployment();
    std::expected<std::string, std::string> configure_desktop_target(
        std::string_view target,
        bool wallet_token);
    std::expected<std::string, std::string> rollback_desktop_target(
        std::string_view target);
    void cancel_camera_operation();

    su::recognizer::RecognizerService& recognizer() { return recognizer_; }

private:
    std::string config_path() const;
    std::string profile_store_path() const;
    su::recognizer::RecognizerService recognizer_{};
    std::atomic<bool> camera_cancel_requested_{false};
};

} // namespace su::app
