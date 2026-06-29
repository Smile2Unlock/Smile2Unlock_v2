#pragma once

#include "recognizer/recognizer_service.h"
#include "app/core_bridge.h"

#include <expected>
#include <string>
#include <vector>

namespace su::app {

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

class AppController {
public:
    std::expected<AppSnapshot, std::string> load_initial_snapshot();
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

private:
    std::string config_path() const;
    std::string profile_store_path() const;
    su::recognizer::RecognizerService recognizer_{};
};

}  // namespace su::app
