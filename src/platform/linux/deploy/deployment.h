#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace su::deploy {

enum class TargetKind {
    kDms,
    kKscreenlocker,
    kPlasmaLogin,
    kGdm,
    kSddm,
    kGreetd,
};

enum class TargetRole {
    kLogin,
    kLock,
    kLoginAndLock,
};

enum class TargetState {
    kAbsent,
    kSupported,
    kManaged,
    kExternal,
    kConflict,
    kUnsupported,
};

struct TargetStatus {
    TargetKind kind;
    TargetRole role;
    TargetState state;
    std::string service;
    std::filesystem::path effective_path;
    bool password_fallback = false;
    bool wallet_modules = false;
    bool wallet_token_enabled = false;
    std::string detail;
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
    bool wallet_token_requested = false;
    bool already_managed = false;
};

[[nodiscard]] std::string_view target_id(TargetKind target);
[[nodiscard]] std::string_view target_service(TargetKind target);
[[nodiscard]] std::string_view target_state_id(TargetState state);
[[nodiscard]] std::optional<TargetKind> target_from_id(std::string_view id);

[[nodiscard]] std::expected<DeploymentSnapshot, std::string> inspect_deployment(
    const std::filesystem::path& root = "/");
[[nodiscard]] std::expected<PamPlan, std::string> plan_pam_integration(
    const std::filesystem::path& root,
    TargetKind target,
    bool wallet_token);
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

[[nodiscard]] std::string snapshot_json(const DeploymentSnapshot& snapshot);
[[nodiscard]] std::string plan_json(const PamPlan& plan);

} // namespace su::deploy
