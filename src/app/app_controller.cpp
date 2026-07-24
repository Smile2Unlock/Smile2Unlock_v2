module;

#include <nlohmann/json.hpp>

module su.app.controller;
import std;
import su.recognizer.service;
import su.recognizer.image;
import su.control.socket;
import su.app.user;

namespace su::app {

namespace {

constexpr std::string_view kImageSourcePrefix = "image:";
constexpr std::string_view kControlSocketPath = su::control::kDefaultSocketPath;
constexpr auto kPamServicePaths = std::array{
    std::string_view{"/etc/pam.d/dankshell-smile2unlock"},
    std::string_view{"/etc/pam.d/greetd"},
};

std::uint64_t next_request_id() {
    static auto sequence = std::atomic<std::uint64_t>{1};
    return sequence.fetch_add(1, std::memory_order_relaxed);
}

std::expected<su::control::ControlResponse, std::string> send_control_request(
    std::string_view request) {
    auto connection = su::control::Connection::connect_to(kControlSocketPath);
    if (!connection) {
        return std::unexpected("authentication service is unavailable");
    }
    if (const auto sent = connection->send_frame(request); !sent) {
        return std::unexpected("failed to send request to authentication service");
    }
    const auto response = connection->receive_frame();
    if (!response) {
        return std::unexpected("authentication service did not respond");
    }
    const auto parsed = su::control::parse_response(*response);
    if (!parsed) {
        return std::unexpected("authentication service returned an invalid response");
    }
    return *parsed;
}

StorageProtection storage_protection_from_response(
    const su::control::ControlResponse& response) {
    if (response.result != su::control::ControlResult::kAccepted) {
        return StorageProtection::kUnavailable;
    }
    const auto payload = nlohmann::json::parse(response.payload_json, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()
        || !payload.contains("protection") || !payload["protection"].is_string()) {
        return StorageProtection::kUnknown;
    }
    const auto protection = payload["protection"].get<std::string>();
    if (protection == "host+tpm2" || protection == "TPM2-bound" || protection == "tpm2") {
        return StorageProtection::kHostTpm2;
    }
    if (protection == "host-key" || protection == "host") {
        return StorageProtection::kHostKey;
    }
    if (protection == "unavailable") {
        return StorageProtection::kUnavailable;
    }
    return StorageProtection::kUnknown;
}

std::pair<bool, std::string> configured_pam_service() {
    for (const auto path_text : kPamServicePaths) {
        const auto path = std::filesystem::path{path_text};
        auto input = std::ifstream{path};
        if (!input) {
            continue;
        }
        for (auto line = std::string{}; std::getline(input, line);) {
            const auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#') {
                continue;
            }
            if (line.find("pam_smile2unlock.so", first) != std::string::npos) {
                return {true, path.filename().string()};
            }
        }
    }
    return {false, {}};
}

std::expected<std::vector<FaceProfileSummary>, std::string> profile_rows_from_json(
    std::string_view payload) {
    const auto parsed = nlohmann::json::parse(payload, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return std::unexpected("authentication service returned invalid profile data");
    }
    auto profiles = std::vector<FaceProfileSummary>{};
    profiles.reserve(parsed.size());
    for (const auto& item : parsed) {
        if (!item.is_object()
            || !item.contains("id") || !item["id"].is_string()
            || !item.contains("label") || !item["label"].is_string()
            || !item.contains("created_at_unix") || !item["created_at_unix"].is_number_unsigned()) {
            return std::unexpected("authentication service returned invalid profile data");
        }
        profiles.push_back(FaceProfileSummary{
            .id = item["id"].get<std::string>(),
            .label = item["label"].get<std::string>(),
            .created_at_unix = item["created_at_unix"].get<std::uint64_t>(),
        });
    }
    return profiles;
}

std::expected<FaceAuthReport, std::string> auth_report_from_json(std::string_view payload) {
    const auto parsed = nlohmann::json::parse(payload, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()
        || !parsed.contains("accepted") || !parsed["accepted"].is_boolean()
        || !parsed.contains("score") || !parsed["score"].is_number()
        || !parsed.contains("threshold") || !parsed["threshold"].is_number()
        || !parsed.contains("liveness_ok") || !parsed["liveness_ok"].is_boolean()
        || !parsed.contains("profile_count") || !parsed["profile_count"].is_number_unsigned()
        || !parsed.contains("best_profile_id") || !parsed["best_profile_id"].is_string()
        || !parsed.contains("best_profile_label") || !parsed["best_profile_label"].is_string()
        || !parsed.contains("reason") || !parsed["reason"].is_string()) {
        return std::unexpected("authentication service returned invalid authentication data");
    }
    return FaceAuthReport{
        .accepted = parsed["accepted"].get<bool>(),
        .score = parsed["score"].get<float>(),
        .threshold = parsed["threshold"].get<float>(),
        .liveness_ok = parsed["liveness_ok"].get<bool>(),
        .profile_count = parsed["profile_count"].get<std::uint32_t>(),
        .best_profile_id = parsed["best_profile_id"].get<std::string>(),
        .best_profile_label = parsed["best_profile_label"].get<std::string>(),
        .reason = parsed["reason"].get<std::string>(),
    };
}

std::string current_account_name() {
    return current_username("");
}

// RAII guard: closes the recognizer camera when leaving scope.
struct [[nodiscard]] CameraGuard {
    su::recognizer::RecognizerService& svc;
    ~CameraGuard() { svc.close_camera(); }
};

bool liveness_passes(const CoreConfig& config, const su::recognizer::RecognitionResult& result) {
    return !config.liveness_detection
        || result.liveness_score >= config.liveness_threshold;
}

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
        if (liveness_passes(config, *result)) {
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
    return (std::filesystem::path{"/var/lib/smile2unlock/users"}
        / std::to_string(current_uid()) / "profiles.s2u").string();
}

std::expected<AppSnapshot, std::string> AppController::load_initial_snapshot() {
    const auto path = config_path();
    const auto loaded_config = load_config(path);
    if (!loaded_config) {
        return std::unexpected(std::format("failed to load config from Rust core: {}", path));
    }
    const auto store_path = profile_store_path();
    auto profiles = std::vector<FaceProfileSummary>{};
    auto profiles_json = std::string{"[]"};
    if (const auto loaded_profiles = list_face_profile_rows(); loaded_profiles) {
        profiles = *loaded_profiles;
        if (const auto loaded_json = list_face_profiles(); loaded_json) {
            profiles_json = *loaded_json;
        }
    } else {
        // Keep the UI available for diagnostics and password fallback while
        // su_authd is stopped or its encrypted key is unavailable.
        std::println(stderr, "[storage] profile list unavailable: {}", loaded_profiles.error());
    }

    return AppSnapshot{
        .title = std::format("Smile2Unlock core v{}", core_version_major()),
        .cameras = recognizer_.enumerate_cameras(),
        .config = *loaded_config,
        .config_path = path,
        .profile_store_path = store_path,
        .profiles = std::move(profiles),
        .profiles_json = std::move(profiles_json),
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

    const auto username = current_account_name();
    if (username.empty()) {
        return std::unexpected("failed to resolve current account");
    }
    const auto response = send_control_request(su::control::make_enroll_profile_request(
        next_request_id(), username, label, *resolved));
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->result != su::control::ControlResult::kAccepted) {
        return std::unexpected(response->reason);
    }
    return response->payload_json;
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

    const auto username = current_account_name();
    if (username.empty()) {
        return std::unexpected("failed to resolve current account");
    }
    const auto response = send_control_request(su::control::make_verify_profile_request(
        next_request_id(), username, *resolved, liveness_ok));
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->result != su::control::ControlResult::kAccepted
        && response->result != su::control::ControlResult::kRejected) {
        return std::unexpected(response->reason);
    }
    const auto auth_report = auth_report_from_json(response->payload_json);
    if (!auth_report) {
        return std::unexpected(auth_report.error());
    }
    const auto profile_rows = list_face_profile_rows();
    if (!profile_rows) {
        return std::unexpected(profile_rows.error());
    }
    const auto profiles_json = list_face_profiles();
    if (!profiles_json) {
        return std::unexpected(profiles_json.error());
    }
    const auto decision = FaceAuthDecision{
        .accepted = auth_report->accepted,
        .score = auth_report->score,
        .profile_count = auth_report->profile_count,
    };

    return FaceDemoSnapshot{
        .profiles = *profile_rows,
        .profiles_json = *profiles_json,
        .auth_report_json = response->payload_json,
        .decision = decision,
        .report = *auth_report,
    };
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
    // CameraGuard closes the camera on every exit path.
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
    const auto username = current_account_name();
    if (username.empty()) {
        return std::unexpected("failed to resolve current account");
    }
    const auto response = send_control_request(su::control::make_list_profiles_request(
        next_request_id(), username));
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->result != su::control::ControlResult::kAccepted) {
        return std::unexpected(response->reason);
    }
    return response->payload_json;
}

std::expected<std::vector<FaceProfileSummary>, std::string> AppController::list_face_profile_rows() {
    const auto profiles = list_face_profiles();
    if (!profiles) {
        return std::unexpected(profiles.error());
    }
    return profile_rows_from_json(*profiles);
}

std::expected<bool, std::string> AppController::delete_face_profile_by_id(std::string_view profile_id) {
    const auto username = current_account_name();
    if (username.empty()) {
        return std::unexpected("failed to resolve current account");
    }
    const auto response = send_control_request(su::control::make_delete_profile_request(
        next_request_id(), username, profile_id));
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->result == su::control::ControlResult::kRejected
        && response->reason == "profile not found") {
        return false;
    }
    if (response->result != su::control::ControlResult::kAccepted) {
        return std::unexpected(response->reason);
    }
    return true;
}

SystemStatus AppController::load_system_status() {
    auto status = SystemStatus{};
    if (const auto response = send_control_request(
            su::control::make_status_request(next_request_id())); response) {
        status.service_available = response->result == su::control::ControlResult::kAccepted;
        status.service_reason = response->reason;
    } else {
        status.service_reason = response.error();
    }

    if (const auto response = send_control_request(
            su::control::make_storage_status_request(next_request_id())); response) {
        status.storage_protection = storage_protection_from_response(*response);
    }

    std::error_code error;
    status.pam_status_known = std::filesystem::is_directory("/etc/pam.d", error) && !error;
    if (status.pam_status_known) {
        auto [configured, service] = configured_pam_service();
        status.pam_configured = configured;
        status.pam_service = std::move(service);
    }
    return status;
}

}  // namespace su::app
