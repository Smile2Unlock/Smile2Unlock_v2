#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

#include "platform/linux/deploy/noctalia.h"

namespace su::deploy {

enum class TargetKind {
    kDms,
    kKscreenlocker,
    kPlasmaLogin,
    kGdm,
    kSddm,
    kGreetd,
    kSudo,
    kSudoLogin,
    kPolkit,
    kNoctalia,
};

enum class TargetRole {
    kLogin,
    kLock,
    kLoginAndLock,
    kPrivilege,
};

enum class TargetState {
    kAbsent,
    kSupported,
    kManaged,
    kExternal,
    kConflict,
    kUnsupported,
};

enum class AuthtokSource { kNone, kKernelKeyring, kSealed };

struct TargetStatus {
    TargetKind kind;
    TargetRole role;
    TargetState state;
    std::string service;
    std::filesystem::path effective_path;
    bool password_fallback = false;
    bool wallet_modules = false;
    bool wallet_token_enabled = false;
    AuthtokSource authtok_source = AuthtokSource::kNone;
    bool sealed_available = false; // Parent AUTH wallet line follows the face/password anchor.
    std::string detail;
    // Translation key for a target-specific explanation; empty when generic.
    std::string note;
    bool adoptable = false;
    std::string adoption_unavailable_reason = "target is not externally configured";
};

struct DeploymentSnapshot {
    std::vector<TargetStatus> targets;
};

struct PamPlan {
    TargetKind target;
    std::string service;
    std::string child_service;
    std::filesystem::path source_path;
    std::filesystem::path destination_path;
    std::filesystem::path child_path;
    std::string source_content;
    std::string destination_content;
    std::string child_content;
    std::string source_fingerprint;
    std::string child_source_content;
    std::string child_source_fingerprint;
    bool destination_existed = false;
    bool child_existed = false;
    AuthtokSource authtok_source = AuthtokSource::kNone;
    bool already_managed = false;
    bool adoption = false;
};

// Inputs a plan needs beyond the target itself. The defaults are the safe,
// privileged ones: the deploy helper passes the D-Bus caller's uid and relies on
// the fixed-prefix helper lookup; only the unprivileged GUI ever supplies an
// environment-derived NoctaliaSearch (and only to inspect_deployment).
struct PlanContext {
    HelperLookup helper = RootHelperLookup{};
    // The account that asked for the change. Required for Noctalia, whose
    // fragment lets an empty password reach PAM.
    std::optional<uid_t> requester_uid;
};

[[nodiscard]] std::string_view target_id(TargetKind target);
[[nodiscard]] std::string_view target_service(TargetKind target);
[[nodiscard]] std::string_view target_role_id(TargetRole role);
[[nodiscard]] std::string_view target_state_id(TargetState state);
[[nodiscard]] std::optional<TargetKind> target_from_id(std::string_view id);

[[nodiscard]] std::expected<DeploymentSnapshot, std::string> inspect_deployment(
    const std::filesystem::path& root = "/",
    const HelperLookup& noctalia_helper = RootHelperLookup{});
[[nodiscard]] std::expected<PamPlan, std::string>
plan_pam_integration(const std::filesystem::path& root, TargetKind target,
                     AuthtokSource authtok_source, const PlanContext& context = {});
[[nodiscard]] std::expected<PamPlan, std::string>
plan_pam_adoption(const std::filesystem::path& root, TargetKind target,
                  AuthtokSource authtok_source = AuthtokSource::kNone,
                  const PlanContext& context = {});
[[nodiscard]] inline std::expected<PamPlan, std::string>
plan_pam_integration(const std::filesystem::path& root, TargetKind target, bool wallet_token,
                     const PlanContext& context = {}) {
    return plan_pam_integration(
        root, target, wallet_token ? AuthtokSource::kKernelKeyring : AuthtokSource::kNone,
        context);
}
[[nodiscard]] std::string_view authtok_source_id(AuthtokSource source);
[[nodiscard]] std::optional<AuthtokSource> authtok_source_from_id(std::string_view id);
[[nodiscard]] std::expected<void, std::string> apply_pam_plan(
    const std::filesystem::path& root,
    const PamPlan& plan);
[[nodiscard]] std::expected<void, std::string> rollback_pam_integration(
    const std::filesystem::path& root,
    TargetKind target);
[[nodiscard]] std::expected<void, std::string> rollback_all_pam_integrations(
    const std::filesystem::path& root = "/");
[[nodiscard]] std::expected<void, std::string> recover_interrupted_pam_transactions(
    const std::filesystem::path& root = "/");
[[nodiscard]] std::expected<void, std::string> check_package_upgrade(
    const std::filesystem::path& root,
    std::string_view candidate_version);

// Service predicate for the Noctalia target; the denied set is derived from the
// target table so it cannot drift.
[[nodiscard]] ServicePolicy noctalia_service_policy();

[[nodiscard]] std::string snapshot_json(const DeploymentSnapshot& snapshot);
// Inverse of snapshot_json, for clients that receive the snapshot over D-Bus.
[[nodiscard]] std::expected<DeploymentSnapshot, std::string> parse_snapshot_json(
    std::string_view text);
[[nodiscard]] std::string plan_json(const PamPlan& plan);

} // namespace su::deploy
