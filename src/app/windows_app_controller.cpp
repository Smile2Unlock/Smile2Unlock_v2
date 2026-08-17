module;

#include <nlohmann/json.hpp>
#include "../platform/windows/deploy/deployment.h"
#include "../platform/windows/udp_recognition_server.h"

module su.app.controller;
import std;
import su.recognizer.service;
import su.recognizer.image;
import su.app.user;
import su.core.types;

// Plain-TU bridge (user_windows.cpp): registry write for the recognition
// policy consumed by the credential provider at lock-screen time.
extern "C" {
int su_win_write_recognition_registry(
    unsigned int mode,
    unsigned int auto_delay_sec,
    unsigned int retry_delay_sec,
    unsigned int timeout_sec);
}

extern "C" int __stdcall ShellExecuteExW(void* exec_info);
extern "C" int __stdcall GetModuleFileNameW(void* module, wchar_t* buffer, unsigned long size);
extern "C" unsigned long __stdcall GetTempPathW(unsigned long length, wchar_t* buffer);
extern "C" int __stdcall WaitForSingleObject(void* handle, unsigned long milliseconds);
extern "C" int __stdcall CloseHandle(void* handle);
extern "C" void* __stdcall CreateFileA(const char* name, unsigned long access, unsigned long share, void* security, unsigned long creation, unsigned long flags, void* template_file);
extern "C" int __stdcall ReadFile(void* file, void* buffer, unsigned long to_read, unsigned long* read, void* overlapped);
extern "C" int __stdcall WideCharToMultiByte(unsigned int code_page, unsigned long flags, const wchar_t* wide, int wide_length, char* narrow, int narrow_length, const char* default_char, int* used_default);

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
// Windows there is no control socket: profile storage is system-owned under
// ProgramData (see docs/rewrite_master_plan.md "凭据存储" platform
// decision) and su_app reads/writes it directly through the Rust core FFI,
// because the Credential Provider never consumes profiles (recognition
// results cross loopback UDP) and the Windows auth service only owns the
// logon-secret pipe.

const char* core_error_name(CoreError error) {
    switch (error) {
        case CoreError::kNullArgument: return "null argument";
        case CoreError::kInvalidUtf8: return "invalid utf-8";
        case CoreError::kUserDenied: return "user denied";
        case CoreError::kIoError: return "io error";
        case CoreError::kParseError: return "parse error";
        case CoreError::kWriteError: return "write error";
        case CoreError::kInvalidArgument: return "invalid argument";
        case CoreError::kBufferTooSmall: return "buffer too small";
        case CoreError::kCryptoError: return "crypto error";
        case CoreError::kKeyUnavailable: return "key unavailable";
        case CoreError::kMigrationRequired: return "migration required";
        case CoreError::kUnknown: return "unknown error";
    }
    return "unknown error";
}

const char* recognizer_error_name(su::recognizer::RecognizerError error) {
    switch (error) {
        case su::recognizer::RecognizerError::kNoCamera: return "no camera";
        case su::recognizer::RecognizerError::kCameraUnavailable: return "camera unavailable";
        case su::recognizer::RecognizerError::kModelUnavailable: return "model unavailable";
        case su::recognizer::RecognizerError::kInvalidArgument: return "invalid argument";
        case su::recognizer::RecognizerError::kInvalidImage: return "invalid image";
        case su::recognizer::RecognizerError::kImageLoadFailed: return "image load failed";
        case su::recognizer::RecognizerError::kNoFace: return "no face";
    }
    return "unknown error";
}

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
    const auto enrolled = su::app::enroll_face_profile(
        profile_store_path(), label, *resolved);
    if (!enrolled) {
        return std::unexpected(std::format("failed to enroll profile: {}", core_error_name(enrolled.error())));
    }
    const auto profiles = su::app::list_face_profiles_json(profile_store_path());
    if (!profiles) {
        return std::unexpected(std::format("failed to list profiles: {}", core_error_name(profiles.error())));
    }
    return *profiles;
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
    const auto config = load_config(config_path());
    if (!config) {
        return std::unexpected(std::format("failed to load config from Rust core: {}", config_path()));
    }
    const auto auth_report = su::app::authenticate_face_sample_report(
        profile_store_path(),
        *resolved,
        config->recognition_threshold,
        liveness_ok);
    if (!auth_report) {
        return std::unexpected(std::format("failed to authenticate face sample: {}", core_error_name(auth_report.error())));
    }
    const auto profiles = su::app::list_face_profiles_json(profile_store_path());
    if (!profiles) {
        return std::unexpected(std::format("failed to list profiles: {}", core_error_name(profiles.error())));
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
    const auto report_json = std::format(
        R"({{"accepted":{},"score":{},"threshold":{},"liveness_ok":{},"profile_count":{},"best_profile_id":"{}","best_profile_label":"{}","reason":"{}"}})",
        auth_report->accepted ? "true" : "false",
        auth_report->score,
        auth_report->threshold,
        auth_report->liveness_ok ? "true" : "false",
        auth_report->profile_count,
        auth_report->best_profile_id,
        auth_report->best_profile_label,
        auth_report->reason);

    return FaceDemoSnapshot{
        .profiles = *profile_rows,
        .profiles_json = *profiles,
        .auth_report_json = report_json,
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
    const auto profiles = su::app::list_face_profiles_json(profile_store_path());
    if (!profiles) {
        return std::unexpected(std::format("failed to list profiles: {}", core_error_name(profiles.error())));
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

std::expected<bool, std::string> AppController::delete_face_profile_by_id(std::string_view profile_id) {
    const auto deleted = su::app::delete_face_profile(profile_store_path(), profile_id);
    if (!deleted) {
        return std::unexpected(std::format("failed to delete profile: {}", core_error_name(deleted.error())));
    }
    return *deleted;
}

SystemStatus AppController::load_system_status() {
    auto status = SystemStatus{};
    status.service_reason = "local profile store (ProgramData)";
    status.storage_protection = StorageProtection::kHostKey;
    const auto snapshot = su::windeploy::inspect_deployment();
    if (snapshot) {
        status.service_available = true;
        for (const auto& target : snapshot->targets) {
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

// Runs su_deploy_helper elevated via UAC (ShellExecuteEx runas) and waits
// for its result file. Returns the helper's JSON result.
std::expected<std::string, std::string> run_elevated_deploy(std::string_view arguments) {
    struct ShellExecuteInfoW {
        unsigned long cb_size;
        unsigned long f_mask;
        void* hwnd;
        const wchar_t* verb;
        const wchar_t* file;
        const wchar_t* parameters;
        const wchar_t* directory;
        int n_show;
        void* h_inst_app;
        void* lp_id_list;
        const wchar_t* lp_class;
        void* hkey_class;
        unsigned long dw_hot_key;
        void* h_icon;
        void* h_process;
    };

    const auto helper = deploy_helper_path();
    if (helper.empty()) {
        return std::unexpected("failed to locate su_deploy_helper.exe");
    }
    const auto wide_helper = std::wstring(helper.begin(), helper.end());
    const auto wide_args = std::wstring(arguments.begin(), arguments.end());

    ShellExecuteInfoW info{};
    info.cb_size = sizeof(info);
    info.f_mask = 0x40;  // SEE_MASK_NOCLOSEPROCESS
    info.verb = L"runas";
    info.file = wide_helper.c_str();
    info.parameters = wide_args.c_str();
    info.n_show = 0;  // SW_HIDE

    if (::ShellExecuteExW(&info) == 0) {
        return std::unexpected("UAC launch of su_deploy_helper was denied or failed");
    }
    if (info.h_process != nullptr) {
        (void)::WaitForSingleObject(info.h_process, 60000);
        (void)::CloseHandle(info.h_process);
    }

    // Read %TEMP%\su_deploy_result.json written by the helper.
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

    void* file = ::CreateFileA(
        result_path.c_str(),
        0x80000000 /* GENERIC_READ */,
        1 /* FILE_SHARE_READ */,
        nullptr,
        3 /* OPEN_EXISTING */,
        0,
        nullptr);
    if (file == nullptr || file == reinterpret_cast<void*>(-1)) {
        return std::unexpected("su_deploy_helper produced no result file");
    }
    char buffer[8192] = {};
    unsigned long read = 0;
    (void)::ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    (void)::CloseHandle(file);
    return std::string(buffer, read);
}

std::expected<void, std::string> deploy_action(std::string_view arguments) {
    if (su::windeploy::process_elevated()) {
        // Already elevated (e.g. launched by the scheduled task): perform
        // the operation directly without a second UAC prompt.
        if (arguments.find("--register-cp") != std::string_view::npos) {
            const auto registered = su::windeploy::register_credential_provider(
                "C:\\su-deploy\\su_credential_provider.dll");
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

// Runs on the UDP recognition server thread: opens the camera, captures a
// live face, authenticates against the local ProgramData profile store and
// reports the outcome to the credential provider.
int AppController::udp_recognize_callback(
    std::uint32_t session_id,
    const char* username_hint,
    char* out_username,
    void* userdata) {
    (void)session_id;
    (void)username_hint;
    auto* self = static_cast<AppController*>(userdata);
    self->camera_cancel_requested_.store(false, std::memory_order_release);

    const auto config = self->load_config_snapshot();
    if (!config) {
        std::println(stderr, "[recognition] udp: failed to load config: {}", config.error());
        return SU_RS_RECOGNITION_ERROR;
    }
    if (!self->recognizer_.seetaface_available()) {
        std::println(stderr, "[recognition] udp: seetaface backend unavailable");
        return SU_RS_RECOGNITION_ERROR;
    }
    if (const auto opened = self->recognizer_.open_camera(config->selected_camera);
        !opened) {
        std::println(stderr, "[recognition] udp: failed to open camera: {}", recognizer_error_name(opened.error()));
        return SU_RS_RECOGNITION_ERROR;
    }
    struct [[nodiscard]] CameraGuard {
        su::recognizer::RecognizerService& svc;
        ~CameraGuard() { svc.close_camera(); }
    };
    const CameraGuard _close_guard{self->recognizer_};

    const auto result = capture_live_features(
        self->recognizer_, *config, self->camera_cancel_requested_);
    if (!result) {
        // No face seen or liveness rejected within the deadline: report as
        // failed so the provider retries per its policy.
        std::println(stderr, "[recognition] udp: capture failed: {}", result.error());
        return SU_RS_FAILED;
    }
    const auto report = su::app::authenticate_face_sample_report(
        self->profile_store_path(),
        su::recognizer::embedding_sample_source(result->feature),
        config->recognition_threshold,
        true);
    if (!report) {
        std::println(stderr, "[recognition] udp: authenticate failed: {}", core_error_name(report.error()));
        return SU_RS_RECOGNITION_ERROR;
    }
    const auto username = su::app::current_username("");
    std::snprintf(out_username, SU_UDP_STATUS_USERNAME_CAP, "%s", username.c_str());
    std::println(stderr, "[recognition] udp: session=0x{:X} user='{}' accepted={} score={:.2f}",
        session_id, username, report->accepted, report->score);
    return report->accepted ? SU_RS_SUCCESS : SU_RS_FAILED;
}

void AppController::start_udp_recognition_server() {
    (void)su_udp_start_recognition_server(&AppController::udp_recognize_callback, this);
}

void AppController::stop_udp_recognition_server() {
    su_udp_stop_recognition_server();
}

}  // namespace su::app
