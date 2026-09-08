module;

#include <nlohmann/json.hpp>
#include <windows.h>
#include <shellapi.h>
#include "../platform/windows/auth_service/profile_client.h"
#include "../platform/windows/deploy/deployment.h"

module su.app.controller;
import std;
import su.recognizer.service;
import su.recognizer.image;
import su.app.user;
import su.core.types;

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
// (su_authd) over the su.control socket and inspects PAM deployments. Windows
// has no control socket: the LocalSystem auth service is the sole
// profile authority. The GUI only sends authenticated profile requests over
// its named pipe; it never opens the protected profile file directly.

std::expected<std::vector<FaceProfileSummary>, std::string> profile_rows_from_json(
    std::string_view payload) {
    const auto parsed = nlohmann::json::parse(payload, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return std::unexpected("profile store returned invalid profile data");
    }
    auto profiles = std::vector<FaceProfileSummary>{};
    profiles.reserve(parsed.size());
    for (const auto& item : parsed) {
        if (!item.is_object()
            || !item.contains("id") || !item["id"].is_string()
            || !item.contains("label") || !item["label"].is_string()
            || !item.contains("created_at_unix") || !item["created_at_unix"].is_number_unsigned()) {
            return std::unexpected("profile store returned invalid profile data");
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
        return std::unexpected("profile store returned invalid authentication data");
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

// Defined below (second anonymous namespace); forward-declared here for
// load_system_status().
std::string deploy_helper_path();

bool save_recognition_settings(const CoreConfig& config) {
    auto raw_key = HKEY{};
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Smile2Unlock\\Recognition",
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &raw_key,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const auto key = std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)>{
        raw_key, &RegCloseKey};
    const auto camera_index = static_cast<DWORD>(std::max(config.selected_camera, 0));
    const auto recognition_threshold = static_cast<DWORD>(
        std::clamp(config.recognition_threshold, 0.5F, 1.0F) * 1000.0F);
    const auto liveness_enabled = DWORD{config.liveness_detection ? 1U : 0U};
    const auto liveness_threshold = static_cast<DWORD>(
        std::clamp(config.liveness_threshold, 0.3F, 1.0F) * 1000.0F);
    const auto set = [raw_key](const wchar_t* name, const DWORD& value) {
        return RegSetValueExW(
            raw_key, name, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
    };
    return set(L"CameraIndex", camera_index)
        && set(L"RecognitionThresholdMilli", recognition_threshold)
        && set(L"LivenessEnabled", liveness_enabled)
        && set(L"LivenessThresholdMilli", liveness_threshold);
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
    return "service://Smile2UnlockAuthService/current-user/profiles";
}

std::vector<su::recognizer::CameraInfo> AppController::enumerate_cameras() const {
    return recognizer_.enumerate_cameras();
}

std::expected<AppSnapshot, std::string> AppController::load_initial_snapshot() {
    const auto path = config_path();
    const auto loaded_config = load_config(path);
    if (!loaded_config) {
        return std::unexpected(std::format("failed to load config from Rust core: {}", path));
    }

    auto profiles_json = std::string{"[]"};
    auto profiles = std::vector<FaceProfileSummary>{};
    if (const auto loaded_json = su::windows::profile_client::list_profiles(); loaded_json) {
        if (const auto rows = profile_rows_from_json(*loaded_json); rows) {
            profiles_json = *loaded_json;
            profiles = *rows;
        }
    }
    return AppSnapshot{
        .title = std::format("Smile2Unlock core v{}", core_version_major()),
        .cameras = recognizer_.enumerate_cameras(),
        .config = *loaded_config,
        .config_path = path,
        .profile_store_path = profile_store_path(),
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
    if (!save_recognition_settings(config)) {
        return std::unexpected("failed to save Windows lock-screen recognition settings");
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
    (void)label;
    (void)face_sample_source;
    return std::unexpected(
        "Windows profile enrollment requires a live camera capture and password verification");
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

    return su::windows::profile_client::enroll_profile(
        label,
        su::recognizer::embedding_sample_source(result->feature));
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
    const auto report_json = su::windows::profile_client::verify_profile(
        *resolved, liveness_ok);
    if (!report_json) {
        return std::unexpected(report_json.error());
    }
    const auto auth_report = auth_report_from_json(*report_json);
    if (!auth_report) {
        return std::unexpected(auth_report.error());
    }
    const auto profiles = su::windows::profile_client::list_profiles();
    if (!profiles) {
        return std::unexpected(profiles.error());
    }
    const auto profile_rows = profile_rows_from_json(*profiles);
    if (!profile_rows) {
        return std::unexpected(profile_rows.error());
    }
    const auto decision = FaceAuthDecision{
        .accepted = auth_report->accepted,
        .score = auth_report->score,
        .profile_count = auth_report->profile_count,
    };
    return FaceDemoSnapshot{
        .profiles = *profile_rows,
        .profiles_json = *profiles,
        .auth_report_json = *report_json,
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
    const auto profiles = su::windows::profile_client::list_profiles();
    if (!profiles) {
        return std::unexpected(profiles.error());
    }
    return *profiles;
}

std::expected<std::vector<FaceProfileSummary>, std::string> AppController::list_face_profile_rows() {
    const auto profiles = list_face_profiles();
    if (!profiles) {
        return std::unexpected(profiles.error());
    }
    return profile_rows_from_json(*profiles);
}

std::expected<bool, std::string> AppController::delete_face_profile_by_id(
    std::string_view profile_id) {
    const auto deleted = su::windows::profile_client::delete_profile(profile_id);
    if (!deleted) {
        return std::unexpected(deleted.error());
    }
    return *deleted;
}

std::expected<void, std::string> AppController::configure_windows_account_credential(
    std::string_view windows_password) {
    return su::windows::profile_client::store_account_credential(windows_password);
}

SystemStatus AppController::load_system_status() {
    auto status = SystemStatus{};
    status.service_reason = "per-user profile store and LocalSystem auth service";
    status.storage_protection = StorageProtection::kHostKey;
    // su_deploy_helper.exe sits next to su_app.exe in the flat deployment
    // layout; report availability so the deployment panel does not ask the
    // user to install a helper that is already deployed.
    status.deployment_helper_available = !deploy_helper_path().empty();
    const auto snapshot = su::windeploy::inspect_deployment();
    if (snapshot) {
        auto credential_provider_ready = false;
        auto auth_service_ready = false;
        for (const auto& target : snapshot->targets) {
            if (target.id == su::windeploy::kCredentialProviderId) {
                credential_provider_ready = target.configured
                    && su::windeploy::credential_provider_registered();
            } else if (target.id == su::windeploy::kAuthServiceId) {
                auth_service_ready = target.configured;
            }
            status.deployment_targets.push_back(DeploymentTargetStatus{
                .id = target.id,
                .service = target.service,
                .effective_path = target.effective_path,
                .role = target.role,
                .state = target.state,
                .detail = target.detail,
                .password_fallback = false,
                .configured = target.configured,
                .configurable = target.configurable,
                .managed = target.managed,
                .wallet_available = target.wallet_available,
                .wallet_enabled = target.wallet_enabled,
            });
        }
        status.service_available = credential_provider_ready && auth_service_ready;
        if (auth_service_ready) {
            if (const auto configured =
                    su::windows::profile_client::account_credential_configured(); configured) {
                status.account_credential_configured = *configured;
            }
        }
    }
    return status;
}

namespace {

// Path of su_deploy_helper.exe next to the current executable.
std::string deploy_helper_path() {
    wchar_t buffer[512] = {};
    const auto length = ::GetModuleFileNameW(nullptr, buffer, 512);
    if (length == 0 || length >= 512) {
        return {};
    }
    std::wstring path(buffer, buffer + length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    path.resize(slash + 1);
    path += L"su_deploy_helper.exe";
    auto narrow = std::string(path.size(), '\0');
    (void)::WideCharToMultiByte(
        65001 /* CP_UTF8 */, 0, path.data(), static_cast<int>(path.size()), narrow.data(),
        static_cast<int>(narrow.size()), nullptr, nullptr);
    return narrow;
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const auto required = ::MultiByteToWideChar(
        65001 /* CP_UTF8 */, 8 /* MB_ERR_INVALID_CHARS */, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    auto wide = std::wstring(static_cast<std::size_t>(required), L'\0');
    if (::MultiByteToWideChar(
            65001 /* CP_UTF8 */, 8 /* MB_ERR_INVALID_CHARS */, value.data(),
            static_cast<int>(value.size()), wide.data(), required) != required) {
        return {};
    }
    return wide;
}

// Runs su_deploy_helper elevated via UAC (ShellExecuteEx runas) and waits
// for its result file. Returns the helper's JSON result.
std::expected<std::string, std::string> run_elevated_deploy(std::string_view arguments) {
    const auto helper = deploy_helper_path();
    if (helper.empty()) {
        return std::unexpected("failed to locate su_deploy_helper.exe");
    }
    const auto wide_helper = utf8_to_wide(helper);
    if (wide_helper.empty()) {
        return std::unexpected("failed to convert the deployment helper path");
    }
    const auto wide_args = std::wstring(arguments.begin(), arguments.end());

    // Remove the previous helper result before launching so a timeout or
    // crash can never be mistaken for this operation's response.
    wchar_t temp[512] = {};
    if (::GetTempPathW(512, temp) == 0) {
        return std::unexpected("failed to resolve the temporary directory");
    }
    auto narrow_temp = std::string(512, '\0');
    const auto converted = ::WideCharToMultiByte(
        65001 /* CP_UTF8 */, 0, temp, -1, narrow_temp.data(), 512, nullptr, nullptr);
    if (converted == 0) {
        return std::unexpected("failed to convert the temporary directory path");
    }
    narrow_temp.resize(std::strlen(narrow_temp.c_str()));
    const auto result_path = narrow_temp + "su_deploy_result.json";
    (void)::DeleteFileA(result_path.c_str());

    auto info = SHELLEXECUTEINFOW{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = wide_helper.c_str();
    info.lpParameters = wide_args.c_str();
    info.nShow = SW_HIDE;

    if (::ShellExecuteExW(&info) == 0) {
        return std::unexpected("UAC launch of su_deploy_helper was denied or failed");
    }
    if (info.hProcess == nullptr) {
        return std::unexpected("UAC helper did not return a process handle");
    }
    const auto wait_result = ::WaitForSingleObject(info.hProcess, 60000);
    (void)::CloseHandle(info.hProcess);
    if (wait_result == WAIT_TIMEOUT) {
        return std::unexpected("su_deploy_helper timed out");
    }
    if (wait_result != WAIT_OBJECT_0) {
        return std::unexpected("failed while waiting for su_deploy_helper");
    }

    const auto file = ::CreateFileA(
        result_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::unexpected("su_deploy_helper produced no result file");
    }
    char buffer[8192] = {};
    unsigned long read = 0;
    (void)::ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    (void)::CloseHandle(file);
    return std::string(buffer, read);
}

std::expected<void, std::string> deploy_action(std::string_view arguments) {
    if (arguments.find("--register-cp") != std::string_view::npos
        || arguments.find("--ensure-service") != std::string_view::npos) {
        const auto validated = su::windeploy::validate_deployment_package();
        if (!validated) {
            return std::unexpected(validated.error());
        }
    }
    if (su::windeploy::process_elevated()) {
        // Already elevated (e.g. launched by the scheduled task): perform
        // the operation directly without a second UAC prompt.
        if (arguments.find("--register-cp") != std::string_view::npos) {
            const auto registered = su::windeploy::register_credential_provider();
            if (!registered) {
                return registered;
            }
        }
        if (arguments.find("--unregister-cp") != std::string_view::npos) {
            const auto unregistered = su::windeploy::unregister_credential_provider();
            if (!unregistered) {
                return unregistered;
            }
        }
        if (arguments.find("--ensure-service") != std::string_view::npos) {
            const auto ensured = su::windeploy::ensure_auth_service();
            if (!ensured) {
                return ensured;
            }
        }
        return {};
    }
    const auto result = run_elevated_deploy(arguments);
    if (!result) {
        return std::unexpected(result.error());
    }
    const auto parsed = nlohmann::json::parse(*result, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("ok") || !parsed["ok"].is_boolean()
        || !parsed["ok"].get<bool>()) {
        const auto detail = parsed.is_object() && parsed.contains("error")
            ? parsed["error"].get<std::string>()
            : std::string("su_deploy_helper reported failure");
        return std::unexpected(detail);
    }
    return {};
}

}  // namespace

std::expected<std::string, std::string> AppController::install_deployment_helper() {
    const auto helper = deploy_helper_path();
    if (helper.empty() || !std::filesystem::exists(helper)) {
        return std::unexpected("su_deploy_helper.exe is not deployed next to su_app.exe");
    }
    return helper;
}

std::expected<std::string, std::string> AppController::initialize_system_deployment() {
    const auto deployed = deploy_action("--register-cp --ensure-service");
    if (!deployed) {
        return std::unexpected(deployed.error());
    }
    return "credential provider registered and auth service started";
}

std::expected<std::string, std::string> AppController::configure_desktop_target(
    std::string_view target,
    bool wallet_token) {
    (void)wallet_token;
    if (target == su::windeploy::kAuthServiceId) {
        const auto configured = deploy_action("--ensure-service");
        if (!configured) {
            return std::unexpected(configured.error());
        }
        return "auth service installed and started";
    }
    if (target != su::windeploy::kCredentialProviderId) {
        return std::unexpected(std::format("unsupported deployment target: {}", target));
    }
    const auto configured = deploy_action("--register-cp");
    if (!configured) {
        return std::unexpected(configured.error());
    }
    return "credential provider registered";
}

std::expected<std::string, std::string> AppController::rollback_desktop_target(
    std::string_view target) {
    if (target != su::windeploy::kCredentialProviderId) {
        return std::unexpected(std::format("unsupported deployment target: {}", target));
    }
    const auto rolled_back = deploy_action("--unregister-cp");
    if (!rolled_back) {
        return std::unexpected(rolled_back.error());
    }
    return "credential provider unregistered";
}

}  // namespace su::app
