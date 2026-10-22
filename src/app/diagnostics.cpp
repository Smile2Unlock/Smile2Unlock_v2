module;
#include "common/diagnostics/export.h"
#include "common/log/logger.h"
#ifdef _WIN32
#include <nlohmann/json.hpp>
#else
#include "platform/linux/deploy_client/deployment_client.h"
#endif
module su.app.controller;
import std;
import su.core.types;
import su.recognizer.types;

namespace su::app {
#ifdef _WIN32
std::expected<std::string, std::string> collect_diagnostics_elevated();
#endif
namespace {
using namespace su::diagnostics;
Summary summary_input(const AppSnapshot& app, const SystemStatus& system,
                      const ModelsStatus& models) {
    auto result = Summary{};
    result.product_version = SU_VERSION_STR;
    result.core_version = std::to_string(core_version_major());
    result.service_available = system.service_available;
    result.service_reason = system.service_reason;
    constexpr std::array names{"unavailable", "host-key", "host-tpm2", "unknown"};
    result.storage_protection = names[static_cast<unsigned>(system.storage_protection)];
    result.recognizer_available = app.seetaface_available;
    for (const auto& camera : app.cameras)
        result.cameras.push_back(camera.name);
    result.profile_count = app.profiles.size();
    result.models_present = models.present;
    result.model_dir = models.model_dir;
    const auto yes = [](bool value) { return value ? "yes" : "no"; };
    result.runtime = {
        {"PAM status known", yes(system.pam_status_known)},
        {"PAM configured", yes(system.pam_configured)},
        {"PAM service", system.pam_service},
        {"Login PAM configured", yes(system.login_pam_configured)},
        {"Lock PAM configured", yes(system.lock_pam_configured)},
        {"Deployment helper available", yes(system.deployment_helper_available)},
        {"Deployment installer available", yes(system.deployment_installer_available)}};
    for (const auto& target : system.deployment_targets) {
        result.targets.push_back({"Target " + target.id,
                                  {{"id", target.id},
                                   {"service", target.service},
                                   {"state", target.state},
                                   {"detail", target.detail},
                                   {"path", target.effective_path},
                                   {"role", target.role},
                                   {"password fallback", yes(target.password_fallback)},
                                   {"configured", yes(target.configured)},
                                   {"configurable", yes(target.configurable)},
                                   {"managed", yes(target.managed)},
                                   {"wallet available", yes(target.wallet_available)},
                                   {"wallet enabled", yes(target.wallet_enabled)}}});
    }
    return result;
}
std::expected<PrivilegedData, Error> collect_privileged() {
#ifdef _WIN32
    const auto response = collect_diagnostics_elevated();
    if (!response)
        return std::unexpected(Error{ErrorCode::denied, response.error()});
    const auto json = nlohmann::json::parse(*response, nullptr, false);
    if (json.is_object() && json.contains("error") && json["error"].is_string())
        return std::unexpected(Error{ErrorCode::denied, json["error"].get<std::string>()});
    if (!json.is_object() || !json.contains("ok") || !json["ok"].is_boolean() ||
        !json["ok"].get<bool>() || !json.contains("staging_directory") ||
        !json["staging_directory"].is_string())
        return std::unexpected(Error{ErrorCode::invalid_result,
                                     "diagnostics helper failed or returned an invalid result"});
    const auto path = validate_windows_staging(json["staging_directory"].get<std::string>());
    if (!path)
        return std::unexpected(path.error());
    auto reason = std::string{};
    if (std::filesystem::exists(*path / "unavailable.txt")) {
        const auto text = read_log_tail(*path / "unavailable.txt");
        if (!text)
            return std::unexpected(text.error());
        reason = *text;
    }
    return PrivilegedData{{}, *path, reason};
#else
    const auto text = su::deploy::DeploymentClient{}.collect_diagnostics();
    if (!text)
        return std::unexpected(Error{ErrorCode::denied, text.error()});
    return PrivilegedData{*text, {}};
#endif
}
} // namespace
std::expected<DiagnosticsExport, std::string> AppController::export_diagnostics() {
    try {
        const auto snapshot = load_initial_snapshot();
        if (!snapshot) {
            su::log::process_logger().error("diagnostics.failed",
                                            {su::log::field("cause", snapshot.error())});
            return std::unexpected(snapshot.error());
        }
        return export_bundle(summary_input(*snapshot, load_system_status(), models_status()),
                             collect_privileged)
            .transform([](const ExportResult& result) {
                return DiagnosticsExport{result.archive_path, result.system_unavailable};
            })
            .transform_error([](const Error& error) { return error.detail; });
    } catch (const std::exception& error) {
        su::log::process_logger().error("diagnostics.failed",
                                        {su::log::field("cause", error.what())});
        return std::unexpected(std::string{error.what()});
    }
}
std::expected<void, std::string> AppController::show_diagnostics_in_folder(std::string_view path) {
    return show_in_folder(path).transform_error([](const Error& error) { return error.detail; });
}
} // namespace su::app
