module;

#include <nlohmann/json.hpp>

module su.app.controller;
import std;
import su.recognizer.service;
import su.recognizer.image;
import su.app.user;

// Plain-TU bridge (user_windows.cpp): registry write for the recognition
// policy consumed by the credential provider at lock-screen time.
extern "C" {
int su_win_write_recognition_registry(
    unsigned int mode,
    unsigned int auto_delay_sec,
    unsigned int retry_delay_sec,
    unsigned int timeout_sec);
}

namespace su::app {

namespace {

constexpr std::string_view kImageSourcePrefix = "image:";

std::expected<su::recognizer::RecognitionResult, std::string> capture_live_features(
    su::recognizer::RecognizerService& recognizer,
    const CoreConfig& config,
    const std::atomic<bool>& cancel_requested) {
    if (cancel_requested.load(std::memory_order_acquire)) {
        return std::unexpected("camera operation cancelled");
    }
    if (config.liveness_detection) {
        if (const auto reset = recognizer.reset_liveness(); !reset) {
            return std::unexpected("failed to reset liveness detector");
        }
    }

    auto saw_face = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{8};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel_requested.load(std::memory_order_acquire)) {
            return std::unexpected("camera operation cancelled");
        }
        const auto result = recognizer.extract_features(config.liveness_detection);
        if (cancel_requested.load(std::memory_order_acquire)) {
            return std::unexpected("camera operation cancelled");
        }
        if (!result || !result->has_face || result->feature.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{80});
            continue;
        }
        saw_face = true;
        const auto liveness_passes = !config.liveness_detection
            || result->liveness_score >= config.liveness_threshold;
        if (liveness_passes) {
            return *result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{80});
    }
    return std::unexpected(saw_face ? "liveness check failed" : "no face detected");
}

// Try to turn an `image:<path>` sample source into a real SeetaFace embedding.
// When the SeetaFace backend is unavailable, the source is returned unchanged
// so the Rust core falls back to its mock image-source handling (which only
// validates the file exists). Returns the embedding:... source on success, or
// an error describing which step failed.
std::expected<std::string, std::string> resolve_image_sample_source(
    su::recognizer::RecognizerService& recognizer,
    std::string_view face_sample_source) {
    if (face_sample_source.size() <= kImageSourcePrefix.size()) {
        return std::string{face_sample_source};
    }
    if (face_sample_source.substr(0, kImageSourcePrefix.size()) != kImageSourcePrefix) {
        return std::string{face_sample_source};
    }
    if (!recognizer.seetaface_available()) {
        // Backend unavailable: let Rust core handle the image: source with its
        // mock backend (file-existence validation only).
        return std::string{face_sample_source};
    }

    const auto path = std::filesystem::path(
        face_sample_source.substr(kImageSourcePrefix.size()));
    auto loaded = su::recognizer::load_image_file(path);
    if (!loaded) {
        return std::unexpected(std::format("failed to load image: {}", path.string()));
    }

    auto result = recognizer.extract_from_image(su::recognizer::ImageView{
        .width = loaded->width,
        .height = loaded->height,
        .channels = loaded->channels,
        .bytes = std::span<const std::byte>(loaded->bytes),
    });
    if (!result) {
        return std::unexpected(std::format("failed to extract face features: {}", path.string()));
    }
    return su::recognizer::embedding_sample_source(result->feature);
}

// The Linux implementation talks to the LocalSystem authentication service
// (su_authd) over the su.control socket and inspects PAM deployments. On
// Windows those services are not available yet: profile storage is owned by
// the Windows auth service (see windows_credential_provider_rust_plan.md).
std::expected<void, std::string> control_unavailable() {
    return std::unexpected("authentication service is unavailable on Windows");
}

}  // namespace

std::string AppController::config_path() const {
    if (const auto* appdata = std::getenv("APPDATA")) {
        return (std::filesystem::path(appdata) / "smile2unlock" / "config.toml").string();
    }
    if (const auto* home = std::getenv("USERPROFILE")) {
        return (std::filesystem::path(home) / "AppData" / "Roaming" / "smile2unlock" / "config.toml").string();
    }
    return (std::filesystem::temp_directory_path() / "smile2unlock" / "config.toml").string();
}

std::string AppController::profile_store_path() const {
    const std::filesystem::path root = [] {
        if (const auto* program_data = std::getenv("PROGRAMDATA")) {
            return std::filesystem::path(program_data);
        }
        return std::filesystem::path{"C:/ProgramData"};
    }();
    return (root / "smile2unlock" / "users"
        / std::to_string(current_uid()) / "profiles.s2u").string();
}

std::expected<AppSnapshot, std::string> AppController::load_initial_snapshot() {
    const auto path = config_path();
    const auto loaded_config = load_config(path);
    if (!loaded_config) {
        return std::unexpected(std::format("failed to load config from Rust core: {}", path));
    }

    return AppSnapshot{
        .title = std::format("Smile2Unlock core v{}", core_version_major()),
        .cameras = recognizer_.enumerate_cameras(),
        .config = *loaded_config,
        .config_path = path,
        .profile_store_path = profile_store_path(),
        .slint_enabled = SU_HAS_SLINT != 0,
        .seetaface_available = recognizer_.seetaface_available(),
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
        true);
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
    // Mirror the recognition trigger policy to the HKLM registry key the
    // credential provider reads. Best-effort: config.toml stays authoritative
    // for the GUI; a non-admin session simply skips the mirror.
    if (su_win_write_recognition_registry(
            config.recognition_mode,
            config.auto_delay_sec,
            config.retry_delay_sec,
            config.timeout_sec) != 0) {
        // Not fatal: recognition settings still apply after the next lock if
        // the registry write succeeds elsewhere; surface nothing here because
        // a non-elevated run is a normal state.
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
    const auto resolved = resolve_image_sample_source(recognizer_, face_sample_source);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    const auto username = current_username("");
    if (username.empty()) {
        return std::unexpected("failed to resolve current account");
    }
    const auto unavailable = control_unavailable();
    return std::unexpected(unavailable.error());
}

std::expected<std::string, std::string> AppController::enroll_face_profile_from_current_frame(
    std::string_view label) {
    camera_cancel_requested_.store(false, std::memory_order_release);
    const auto config = load_config(config_path());
    if (!config) {
        return std::unexpected(std::format("failed to load config from Rust core: {}", config_path()));
    }
    if (const auto opened = recognizer_.open_camera(config->selected_camera); !opened) {
        return std::unexpected("failed to open configured camera");
    }
    // CameraGuard closes the camera on every exit path.
    struct [[nodiscard]] CameraGuard {
        su::recognizer::RecognizerService& svc;
        ~CameraGuard() { svc.close_camera(); }
    };
    const CameraGuard _close_guard{recognizer_};
    const auto result = capture_live_features(recognizer_, *config, camera_cancel_requested_);
    if (!result) {
        return std::unexpected(result.error());
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
    const auto resolved = resolve_image_sample_source(recognizer_, face_sample_source);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    const auto username = current_username("");
    if (username.empty()) {
        return std::unexpected("failed to resolve current account");
    }
    const auto unavailable = control_unavailable();
    return std::unexpected(unavailable.error());
}

std::expected<FaceDemoSnapshot, std::string> AppController::authenticate_current_frame() {
    camera_cancel_requested_.store(false, std::memory_order_release);
    const auto config = load_config(config_path());
    if (!config) {
        return std::unexpected(std::format("failed to load config from Rust core: {}", config_path()));
    }
    if (const auto opened = recognizer_.open_camera(config->selected_camera); !opened) {
        return std::unexpected("failed to open configured camera");
    }
    struct [[nodiscard]] CameraGuard {
        su::recognizer::RecognizerService& svc;
        ~CameraGuard() { svc.close_camera(); }
    };
    const CameraGuard _close_guard{recognizer_};
    const auto result = capture_live_features(recognizer_, *config, camera_cancel_requested_);
    if (!result) {
        return std::unexpected(result.error());
    }

    return authenticate_face_sample_from_source(
        su::recognizer::embedding_sample_source(result->feature),
        true);
}

void AppController::cancel_camera_operation() {
    camera_cancel_requested_.store(true, std::memory_order_release);
    recognizer_.close_camera();
}

std::expected<std::string, std::string> AppController::list_face_profiles() {
    const auto unavailable = control_unavailable();
    return std::unexpected(unavailable.error());
}

std::expected<std::vector<FaceProfileSummary>, std::string> AppController::list_face_profile_rows() {
    const auto unavailable = control_unavailable();
    return std::unexpected(unavailable.error());
}

std::expected<bool, std::string> AppController::delete_face_profile_by_id(std::string_view profile_id) {
    const auto unavailable = control_unavailable();
    return std::unexpected(unavailable.error());
}

SystemStatus AppController::load_system_status() {
    auto status = SystemStatus{};
    status.service_reason = "authentication service is unavailable on Windows";
    return status;
}

std::expected<std::string, std::string> AppController::install_deployment_helper() {
    return std::unexpected("desktop deployment is only supported on Linux");
}

std::expected<std::string, std::string> AppController::initialize_system_deployment() {
    return std::unexpected("desktop deployment is only supported on Linux");
}

std::expected<std::string, std::string> AppController::configure_desktop_target(
    std::string_view target,
    bool wallet_token) {
    return std::unexpected("desktop deployment is only supported on Linux");
}

std::expected<std::string, std::string> AppController::rollback_desktop_target(
    std::string_view target) {
    return std::unexpected("desktop deployment is only supported on Linux");
}

}  // namespace su::app
