#include "app/app_controller.h"

#include <cstdlib>
#include <format>
#include <filesystem>

namespace su::app {

namespace {

constexpr auto kCurrentFrameLivenessThreshold = 0.50F;

bool liveness_passes(const CoreConfig& config, const su::recognizer::RecognitionResult& result) {
    return !config.liveness_detection || result.liveness_score >= kCurrentFrameLivenessThreshold;
}

}  // namespace

std::string AppController::config_path() const {
    if (const auto* xdg_config_home = std::getenv("XDG_CONFIG_HOME")) {
        return (std::filesystem::path(xdg_config_home) / "smile2unlock" / "config.toml").string();
    }
    if (const auto* home = std::getenv("HOME")) {
        return (std::filesystem::path(home) / ".config" / "smile2unlock" / "config.toml").string();
    }
    return (std::filesystem::temp_directory_path() / "smile2unlock" / "config.toml").string();
}

std::string AppController::profile_store_path() const {
    if (const auto* xdg_data_home = std::getenv("XDG_DATA_HOME")) {
        return (std::filesystem::path(xdg_data_home) / "smile2unlock" / "profiles.json").string();
    }
    if (const auto* home = std::getenv("HOME")) {
        return (std::filesystem::path(home) / ".local" / "share" / "smile2unlock" / "profiles.json").string();
    }
    return (std::filesystem::temp_directory_path() / "smile2unlock" / "profiles.json").string();
}

std::expected<AppSnapshot, std::string> AppController::load_initial_snapshot() {
    const auto path = config_path();
    const auto loaded_config = load_config(path);
    if (!loaded_config) {
        return std::unexpected(std::format("failed to load config from Rust core: {}", path));
    }
    if (const auto saved = save_config(path, *loaded_config); !saved) {
        return std::unexpected(std::format("failed to save config through Rust core: {}", path));
    }
    const auto store_path = profile_store_path();
    const auto profiles = list_face_profile_summaries(store_path);
    if (!profiles) {
        return std::unexpected(std::format("failed to list face profiles: {}", profile_store_path()));
    }
    const auto profiles_json = list_face_profiles_json(store_path);
    if (!profiles_json) {
        return std::unexpected(std::format("failed to list face profiles json: {}", store_path));
    }

    return AppSnapshot{
        .title = std::format("Smile2Unlock core v{}", core_version_major()),
        .cameras = recognizer_.enumerate_cameras(),
        .config = *loaded_config,
        .config_path = path,
        .profile_store_path = store_path,
        .profiles = *profiles,
        .profiles_json = *profiles_json,
        .slint_enabled = SU_HAS_SLINT != 0,
    };
}

std::expected<bool, std::string> AppController::evaluate_demo_auth(std::string_view username) {
    const auto path = config_path();
    const auto loaded_config = load_config(path);
    if (!loaded_config) {
        return std::unexpected(std::format("failed to load config from Rust core: {}", path));
    }

    const auto decision = evaluate_auth(
        username,
        0.72F,
        loaded_config->recognition_threshold,
        loaded_config->liveness_detection);
    if (!decision) {
        return std::unexpected("Rust core rejected the demo auth request");
    }

    return decision->accepted;
}

std::expected<CoreConfig, std::string> AppController::load_config_snapshot() {
    const auto path = config_path();
    const auto loaded_config = load_config(path);
    if (!loaded_config) {
        return std::unexpected(std::format("failed to load config from Rust core: {}", path));
    }
    return *loaded_config;
}

std::expected<void, std::string> AppController::save_config_snapshot(const CoreConfig& config) {
    const auto path = config_path();
    const auto saved = save_config(path, config);
    if (!saved) {
        return std::unexpected(std::format("failed to save config through Rust core: {}", path));
    }
    return {};
}

std::expected<FaceDemoSnapshot, std::string> AppController::run_face_demo(
    std::string_view label,
    std::string_view enroll_sample_source,
    std::string_view probe_sample_source) {
    const auto enrolled_profiles = enroll_face_profile_from_sample(label, enroll_sample_source);
    if (!enrolled_profiles) {
        return std::unexpected(enrolled_profiles.error());
    }
    return authenticate_face_sample_from_source(probe_sample_source);
}

std::expected<std::string, std::string> AppController::enroll_face_profile_from_sample(
    std::string_view label,
    std::string_view face_sample_source) {
    const auto store_path = profile_store_path();
    if (const auto enrolled = enroll_face_profile(store_path, label, face_sample_source); !enrolled) {
        return std::unexpected(std::format("failed to enroll face profile: {}", store_path));
    }

    const auto profiles = list_face_profiles_json(store_path);
    if (!profiles) {
        return std::unexpected(std::format("failed to list face profiles: {}", store_path));
    }
    return *profiles;
}

std::expected<std::string, std::string> AppController::enroll_face_profile_from_current_frame(
    std::string_view label) {
    const auto config = load_config(config_path());
    if (!config) {
        return std::unexpected(std::format("failed to load config from Rust core: {}", config_path()));
    }
    if (const auto opened = recognizer_.open_camera(config->selected_camera); !opened) {
        return std::unexpected("failed to open configured camera");
    }
    const auto result = recognizer_.extract_features();
    if (!result || !result->has_face || result->feature.empty()) {
        return std::unexpected("failed to extract face features from current frame");
    }

    return enroll_face_profile_from_sample(label, su::recognizer::embedding_sample_source(result->feature));
}

std::expected<FaceDemoSnapshot, std::string> AppController::authenticate_face_sample_from_source(
    std::string_view face_sample_source) {
    return authenticate_face_sample_from_source(face_sample_source, true);
}

std::expected<FaceDemoSnapshot, std::string> AppController::authenticate_face_sample_from_source(
    std::string_view face_sample_source,
    bool liveness_ok) {
    const auto store_path = profile_store_path();
    const auto config = load_config(config_path());
    if (!config) {
        return std::unexpected(std::format("failed to load config from Rust core: {}", config_path()));
    }

    const auto decision = authenticate_face_sample(
        store_path,
        face_sample_source,
        config->recognition_threshold,
        liveness_ok);
    if (!decision) {
        return std::unexpected(std::format("failed to authenticate face sample: {}", store_path));
    }

    const auto report = authenticate_face_sample_report_json(
        store_path,
        face_sample_source,
        config->recognition_threshold,
        liveness_ok);
    if (!report) {
        return std::unexpected(std::format("failed to build face auth report: {}", store_path));
    }

    const auto profiles = list_face_profiles_json(store_path);
    if (!profiles) {
        return std::unexpected(std::format("failed to list face profiles: {}", store_path));
    }
    const auto profile_rows = list_face_profile_summaries(store_path);
    if (!profile_rows) {
        return std::unexpected(std::format("failed to list face profile summaries: {}", store_path));
    }
    const auto auth_report = authenticate_face_sample_report(
        store_path,
        face_sample_source,
        config->recognition_threshold,
        liveness_ok);
    if (!auth_report) {
        return std::unexpected(std::format("failed to build structured face auth report: {}", store_path));
    }

    return FaceDemoSnapshot{
        .profiles = *profile_rows,
        .profiles_json = *profiles,
        .auth_report_json = *report,
        .decision = *decision,
        .report = *auth_report,
    };
}

std::expected<FaceDemoSnapshot, std::string> AppController::authenticate_current_frame() {
    const auto config = load_config(config_path());
    if (!config) {
        return std::unexpected(std::format("failed to load config from Rust core: {}", config_path()));
    }
    if (const auto opened = recognizer_.open_camera(config->selected_camera); !opened) {
        return std::unexpected("failed to open configured camera");
    }
    const auto result = recognizer_.extract_features();
    if (!result || !result->has_face || result->feature.empty()) {
        return std::unexpected("failed to extract face features from current frame");
    }

    const auto liveness_ok = liveness_passes(*config, *result);
    return authenticate_face_sample_from_source(
        su::recognizer::embedding_sample_source(result->feature),
        liveness_ok);
}

std::expected<std::string, std::string> AppController::list_face_profiles() {
    const auto store_path = profile_store_path();
    const auto profiles = list_face_profiles_json(store_path);
    if (!profiles) {
        return std::unexpected(std::format("failed to list face profiles: {}", store_path));
    }
    return *profiles;
}

std::expected<std::vector<FaceProfileSummary>, std::string> AppController::list_face_profile_rows() {
    const auto store_path = profile_store_path();
    const auto profiles = list_face_profile_summaries(store_path);
    if (!profiles) {
        return std::unexpected(std::format("failed to list face profile summaries: {}", store_path));
    }
    return *profiles;
}

std::expected<bool, std::string> AppController::delete_face_profile_by_id(std::string_view profile_id) {
    const auto store_path = profile_store_path();
    const auto deleted = delete_face_profile(store_path, profile_id);
    if (!deleted) {
        return std::unexpected(std::format("failed to delete face profile: {}", store_path));
    }
    return *deleted;
}

}  // namespace su::app
