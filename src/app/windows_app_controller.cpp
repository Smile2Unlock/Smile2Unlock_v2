module;

#include <nlohmann/json.hpp>
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <wincrypt.h>
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
    const auto trigger_saved =
        set(L"CameraIndex", camera_index)
        && set(L"RecognitionThresholdMilli", recognition_threshold)
        && set(L"LivenessEnabled", liveness_enabled)
        && set(L"LivenessThresholdMilli", liveness_threshold)
        && set(L"RecognitionMode", static_cast<DWORD>(config.recognition_mode))
        && set(L"AutoDelaySec", static_cast<DWORD>(config.auto_delay_sec))
        && set(L"RetryDelaySec", static_cast<DWORD>(config.retry_delay_sec))
        && set(L"TimeoutSec", static_cast<DWORD>(config.timeout_sec));
    if (!trigger_saved) {
        return false;
    }
    // The credential provider runs as SYSTEM in LogonUI and reads the
    // machine-wide copy written by the service, which survives a cold boot
    // where HKEY_USERS\<SID> is not loaded. The service is a required part of
    // the Windows deployment, so a failed push is a real save failure.
    const auto pushed = su::windows::profile_client::store_recognition_settings(
        su::windows::profile_client::RecognitionSettings{
            .camera_index = camera_index,
            .recognition_threshold_milli = recognition_threshold,
            .liveness_enabled = liveness_enabled,
            .liveness_threshold_milli = liveness_threshold,
            .recognition_mode = static_cast<std::uint32_t>(config.recognition_mode),
            .auto_delay_sec = static_cast<std::uint32_t>(config.auto_delay_sec),
            .retry_delay_sec = static_cast<std::uint32_t>(config.retry_delay_sec),
            .timeout_sec = static_cast<std::uint32_t>(config.timeout_sec),
        });
    return pushed.has_value();
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
    // Smile2UnlockDeployHelper.exe sits next to Smile2Unlock.exe in the flat deployment
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

// Path of Smile2UnlockDeployHelper.exe next to the current executable.
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
    path += L"Smile2UnlockDeployHelper.exe";
    const auto required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    auto narrow = std::string(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
            narrow.data(), required, nullptr, nullptr) != required) {
        return {};
    }
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
        return std::unexpected("failed to locate Smile2UnlockDeployHelper.exe");
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
        return std::unexpected("Smile2UnlockDeployHelper.exe is not deployed next to Smile2Unlock.exe");
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


// ---------------------------------------------------------------------------
// Face-model download (SeetaFace6 archive from the models GitHub release).
// ---------------------------------------------------------------------------
namespace {

// Mirrors the recognizer backend's model-dir resolution: walk up from the
// executable looking for assets/models/seeta.
constexpr auto kRequiredModels = std::array{
    L"face_detector.csta", L"face_landmarker_pts5.csta", L"face_recognizer.csta",
    L"fas_first.csta", L"fas_second.csta",
};

constexpr char kModelsAssetUrl[] =
    "https://github.com/Smile2Unlock/Smile2Unlock_v2/releases/download/"
    "models-seetaface6-v1/smile2unlock-models-seetaface6-v1.zip";

std::filesystem::path expected_model_dir() {
    wchar_t exe[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
        return {};
    }
    auto dir = std::filesystem::path(exe).parent_path();
    for (int depth = 0; depth < 6 && dir.has_parent_path(); ++depth) {
        const auto candidate = dir / "assets" / "models" / "seeta";
        std::error_code ignored{};
        if (std::filesystem::is_directory(candidate, ignored)) {
            return candidate;
        }
        dir = dir.parent_path();
    }
    return std::filesystem::path(exe).parent_path() / "assets" / "models" / "seeta";
}

std::expected<std::string, std::string> http_download_to_file(
    const std::string& url, const std::filesystem::path& dest,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    if (url.rfind("https://", 0) != 0) {
        return std::unexpected("only https:// download URLs are supported");
    }
    const auto authority = url.substr(8);
    const auto slash = authority.find('/');
    if (slash == std::string::npos) {
        return std::unexpected("invalid download URL");
    }
    const auto host = utf8_to_wide(authority.substr(0, slash));
    const auto path = utf8_to_wide(authority.substr(slash));
    if (host.empty()) {
        return std::unexpected("invalid download URL");
    }

    const auto session = ::WinHttpOpen(
        L"Smile2Unlock/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        return std::unexpected("failed to open the HTTP session");
    }
    const auto close_session = std::unique_ptr<std::remove_pointer_t<HINTERNET>, decltype(&::WinHttpCloseHandle)>{
        session, &::WinHttpCloseHandle};
    const auto connection = ::WinHttpConnect(
        session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection == nullptr) {
        return std::unexpected("failed to connect to the download host");
    }
    const auto close_connection = std::unique_ptr<std::remove_pointer_t<HINTERNET>, decltype(&::WinHttpCloseHandle)>{
        connection, &::WinHttpCloseHandle};
    const auto request = ::WinHttpOpenRequest(
        connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request == nullptr) {
        return std::unexpected("failed to open the download request");
    }
    const auto close_request = std::unique_ptr<std::remove_pointer_t<HINTERNET>, decltype(&::WinHttpCloseHandle)>{
        request, &::WinHttpCloseHandle};
    if (!::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !::WinHttpReceiveResponse(request, nullptr)) {
        return std::unexpected("the download request failed (network or mirror unreachable)");
    }

    auto status_code = DWORD{};
    DWORD status_size = sizeof(status_code);
    if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size, WINHTTP_NO_HEADER_INDEX)
        || status_code != 200) {
        return std::unexpected("the download server returned status "
            + std::to_string(status_code));
    }

    DWORD total = 0;
    DWORD length_size = sizeof(total);
    if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &total, &length_size, WINHTTP_NO_HEADER_INDEX)) {
        total = 0;
    }

    const auto file = ::CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::unexpected("failed to create the download file");
    }
    const auto close_file = std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>{
        file, &::CloseHandle};

    auto buffer = std::array<char, 64 * 1024>{};
    std::uint64_t done = 0;
    for (;;) {
        auto read = DWORD{};
        if (!::WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            return std::unexpected("the download was interrupted");
        }
        if (read == 0) {
            break;
        }
        auto written = DWORD{};
        if (!::WriteFile(file, buffer.data(), read, &written, nullptr) || written != read) {
            return std::unexpected("failed to write the download file");
        }
        done += read;
        if (progress) {
            progress(done, total);
        }
    }
    if (total != 0 && done != total) {
        return std::unexpected("the download ended early");
    }
    return std::string{};
}

std::expected<std::string, std::string> sha256_file_hex(const std::filesystem::path& path) {
    const auto file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::unexpected("failed to open the downloaded archive");
    }
    const auto close_file = std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>{
        file, &::CloseHandle};
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!::CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)
        || !::CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        if (hash != 0) ::CryptDestroyHash(hash);
        if (provider != 0) ::CryptReleaseContext(provider, 0);
        return std::unexpected("failed to hash the downloaded archive");
    }
    auto buffer = std::array<char, 64 * 1024>{};
    for (;;) {
        auto read = DWORD{};
        if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) {
            break;
        }
        if (!::CryptHashData(hash, reinterpret_cast<const BYTE*>(buffer.data()), read, 0)) {
            ::CryptDestroyHash(hash);
            ::CryptReleaseContext(provider, 0);
            return std::unexpected("failed to hash the downloaded archive");
        }
    }
    auto digest = std::array<BYTE, 32>{};
    DWORD digest_size = static_cast<DWORD>(digest.size());
    const auto ok = ::CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digest_size, 0);
    ::CryptDestroyHash(hash);
    ::CryptReleaseContext(provider, 0);
    if (!ok) {
        return std::unexpected("failed to hash the downloaded archive");
    }
    auto hex = std::string{};
    hex.reserve(digest.size() * 2);
    static constexpr auto kHex = "0123456789abcdef";
    for (const auto byte : digest) {
        hex.push_back(kHex[byte >> 4]);
        hex.push_back(kHex[byte & 0xF]);
    }
    return hex;
}

// Windows 10 1803+ ships bsdtar as tar.exe; it extracts zip archives, so no
// third-party unpacker is needed.
std::expected<std::string, std::string> extract_archive_with_tar(
    const std::filesystem::path& archive, const std::filesystem::path& destination) {
    std::error_code ignored{};
    std::filesystem::create_directories(destination, ignored);
    auto command = std::format(
        L"tar -xf \"{}\" -C \"{}\"", archive.wstring(), destination.wstring());
    auto startup = STARTUPINFOW{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    auto process = PROCESS_INFORMATION{};
    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return std::unexpected("failed to launch the archive extractor (tar.exe)");
    }
    const auto close_process = std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>{
        process.hProcess, &::CloseHandle};
    const auto close_thread = std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>{
        process.hThread, &::CloseHandle};
    if (::WaitForSingleObject(process.hProcess, 120'000) != WAIT_OBJECT_0) {
        ::TerminateProcess(process.hProcess, 1);
        return std::unexpected("the archive extraction timed out");
    }
    auto exit_code = DWORD{};
    if (!::GetExitCodeProcess(process.hProcess, &exit_code) || exit_code != 0) {
        return std::unexpected("the archive extraction failed");
    }
    return std::string{};
}

} // namespace

ModelsStatus AppController::models_status() {
    auto status = ModelsStatus{};
    const auto dir = expected_model_dir();
    status.model_dir = dir.string();
    auto present = recognizer_.seetaface_available();
    if (present) {
        for (const auto* name : kRequiredModels) {
            std::error_code ignored{};
            present = present && std::filesystem::exists(dir / name, ignored);
        }
    }
    status.present = present;
    return status;
}

void AppController::download_models(
    const std::string& url_prefix,
    std::function<void(std::uint64_t, std::uint64_t)> progress,
    std::function<void(std::string)> finished) {
    std::thread([url_prefix, progress = std::move(progress), finished = std::move(finished)]() mutable {
        const auto fail = [&finished](std::string message) {
            if (finished) {
                finished(std::move(message));
            }
        };
        const auto model_dir = expected_model_dir();
        const auto models_parent = model_dir.parent_path(); // .../assets/models
        wchar_t temp[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, temp) == 0) {
            fail("failed to resolve the temporary directory");
            return;
        }
        const auto archive = std::filesystem::path(temp) / "smile2unlock-models.zip";
        const auto archive_hash_file = std::filesystem::path(temp) / "smile2unlock-models.zip.sha256";
        const auto archive_url = url_prefix.empty()
            ? std::string{kModelsAssetUrl}
            : url_prefix + kModelsAssetUrl;
        const auto hash_url = archive_url + ".sha256";

        // The archive dominates the transfer; the tiny .sha256 rides the last
        // few percent so the progress bar keeps moving to the end.
        const auto archive_progress =
            [&progress](std::uint64_t done, std::uint64_t total) {
                if (progress && total != 0) {
                    progress(done * 95 / total, total);
                }
            };
        if (const auto downloaded = http_download_to_file(archive_url, archive, archive_progress); !downloaded) {
            fail(downloaded.error());
            return;
        }
        if (progress) {
            progress(96, 100);
        }
        if (const auto downloaded = http_download_to_file(hash_url, archive_hash_file, nullptr); !downloaded) {
            fail("failed to fetch the checksum file: " + downloaded.error());
            return;
        }
        if (progress) {
            progress(97, 100);
        }
        // The .sha256 sidecar carries "<hex>  <filename>".
        auto hash_in = std::ifstream{archive_hash_file};
        auto expected_hex = std::string{};
        hash_in >> expected_hex;
        std::ranges::transform(expected_hex, expected_hex.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        const auto actual = sha256_file_hex(archive);
        if (!actual) {
            fail(actual.error());
            return;
        }
        if (*actual != expected_hex) {
            std::error_code ignored{};
            std::filesystem::remove(archive, ignored);
            fail("checksum mismatch; the download was corrupted, please retry");
            return;
        }
        if (progress) {
            progress(98, 100);
        }
        if (const auto extracted = extract_archive_with_tar(archive, models_parent); !extracted) {
            fail(extracted.error());
            return;
        }
        std::error_code ignored{};
            std::filesystem::remove(archive, ignored);
        std::filesystem::remove(archive_hash_file, ignored);
        // The archive nests seeta/*.csta, so extracting into the models
        // parent lands the files directly in the expected directory.
        for (const auto* name : kRequiredModels) {
            std::error_code ignored{};
            if (!std::filesystem::exists(model_dir / name, ignored)) {
                fail("the extracted archive is missing " + std::filesystem::path(name).string());
                return;
            }
        }
        if (progress) {
            progress(100, 100);
        }
        if (finished) {
            finished({});
        }
    }).detach();
}

}  // namespace su::app
