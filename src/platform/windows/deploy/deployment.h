// Plain (non-module) TU: winreg.h etc. are safe to include here.
//
// Windows deployment/integration library, the counterpart of the Linux
// su_deploy library. Owns the credential-provider registration (CLSID +
// CredentialProviders logon-UI enrollment) and the auth-service lifecycle
// checks. The GUI (AppController) runs these either directly when already
// elevated, or through su_deploy_helper.exe relaunched with UAC.

#ifndef SU_PLATFORM_WINDOWS_DEPLOY_DEPLOYMENT_H_
#define SU_PLATFORM_WINDOWS_DEPLOY_DEPLOYMENT_H_

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace su::windeploy {

// A deployable Windows component shown in the GUI deployment panel.
struct ComponentStatus {
    std::string id;
    std::string service;
    std::string effective_path;
    std::string role;   // "login" | "lock" | "login-and-lock"
    std::string state;  // "managed" | "absent" | "supported" | "external"
    std::string detail;
    bool configured = false;
    bool configurable = false;
    bool managed = false;
    bool wallet_available = false;
    bool wallet_enabled = false;
};

struct DeploymentSnapshot {
    std::vector<ComponentStatus> targets;
};

// Component ids understood by configure/rollback.
inline constexpr std::string_view kCredentialProviderId = "credential-provider";
inline constexpr std::string_view kAuthServiceId = "auth-service";

// True when the current process runs with an elevated (administrator) token.
[[nodiscard]] bool process_elevated();

// Registry: is the credential provider enrolled in the logon UI?
// (CredentialProviders key contains the CLSID.)
[[nodiscard]] bool credential_provider_enrolled();

// Registry: is the CLSID registered and pointing at an existing DLL?
[[nodiscard]] bool credential_provider_registered();

// Path of the DLL the CLSID points at (empty when unregistered).
[[nodiscard]] std::string credential_provider_dll_path();

// Auth service: installed (sc exists) and running?
[[nodiscard]] bool auth_service_installed();
[[nodiscard]] bool auth_service_running();
[[nodiscard]] std::string auth_service_binary_path();

// Full snapshot for the GUI deployment panel. Read-only; no elevation needed.
[[nodiscard]] std::expected<DeploymentSnapshot, std::string> inspect_deployment();

// Elevation-required operations. On failure the error names the failing step.
[[nodiscard]] std::expected<void, std::string> register_credential_provider(
    const std::string& dll_path);
[[nodiscard]] std::expected<void, std::string> unregister_credential_provider();
[[nodiscard]] std::expected<void, std::string> ensure_auth_service();

// Snapshot as JSON (same shape as the UI's DeploymentTargetStatus rows).
[[nodiscard]] std::string snapshot_json(const DeploymentSnapshot& snapshot);

}  // namespace su::windeploy

#endif  // SU_PLATFORM_WINDOWS_DEPLOY_DEPLOYMENT_H_
