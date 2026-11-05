#include "platform/linux/deploy/deployment.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <array>
#include <algorithm>
#include <format>
#include <iostream>

namespace {
void write(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << content;
}
std::string read(const std::filesystem::path& path) {
    auto input = std::ifstream(path);
    return {std::istreambuf_iterator<char>(input), {}};
}
std::string fingerprint(std::string_view content) {
    auto value = std::uint64_t{14695981039346656037ULL};
    for (const auto byte : content) {
        value ^= static_cast<unsigned char>(byte);
        value *= 1099511628211ULL;
    }
    return std::format("{:016x}", value);
}
} // namespace

bool sealed_deployment_tests(const std::filesystem::path& base) {
    using namespace su::deploy;
    auto passed = true;
    const auto check = [&](bool ok, std::string_view message) {
        if (!ok) {
            std::cerr << "FAIL: " << message << '\n';
            passed = false;
        }
    };
    const auto fixtures = std::array{
        std::pair{"arch", "auth include system-login\n-auth optional pam_gnome_keyring.so\n"},
        std::pair{"fedora", "auth required pam_env.so\nauth substack password-auth\nauth optional "
                            "pam_kwallet5.so\nauth include postlogin\n"},
        std::pair{"debian-ubuntu", "@include common-auth\nauth optional pam_gnome_keyring.so\n"},
        std::pair{"opensuse", "auth include common-auth-pc\n-auth optional pam_kwallet5.so\n"},
        std::pair{
            "absolute-module",
            "auth include system-login\n-auth optional /usr/lib/security/pam_gnome_keyring.so\n"},
    };
    for (const auto& [name, auth] : fixtures) {
        const auto root = base / name;
        const auto parent = root / "etc/pam.d/gdm-password";
        const auto original =
            std::string(auth) +
            "account required pam_permit.so\nsession optional pam_gnome_keyring.so auto_start\n";
        write(parent, original);
        const auto initial_status = inspect_deployment(root);
        check(initial_status && initial_status->targets[3].sealed_available,
              "unmanaged supported target exposes sealed eligibility");
        // Exercise every directed transition, not just returning through none.
        for (const auto initial :
             {AuthtokSource::kNone, AuthtokSource::kKernelKeyring, AuthtokSource::kSealed}) {
            auto plan = plan_pam_integration(root, TargetKind::kGdm, initial);
            check(plan && apply_pam_plan(root, *plan), "initial source applies");
            for (const auto next :
                 {AuthtokSource::kKernelKeyring, AuthtokSource::kSealed, AuthtokSource::kNone,
                  AuthtokSource::kSealed, AuthtokSource::kKernelKeyring, initial}) {
                plan = plan_pam_integration(root, TargetKind::kGdm, next);
                check(plan.has_value(), "source transition plans");
                if (!plan)
                    continue;
                if (next == AuthtokSource::kSealed) {
                    check(plan->child_content.find("auth sufficient pam_smile2unlock.so "
                                                   "socket=/run/smile2unlock/control.sock "
                                                   "authtok=sealed\nauth substack ") !=
                              std::string::npos,
                          "sealed child uses auth-phase injection and original password fallback");
                    check(plan->child_content.find("session") == std::string::npos,
                          "no session transaction");
                }
                check(apply_pam_plan(root, *plan).has_value(), "source transition applies");
                const auto snapshot = inspect_deployment(root);
                check(snapshot && snapshot->targets[3].authtok_source == next,
                      "status reports actual source");
                check(snapshot && snapshot->targets[3].sealed_available,
                      "managed status exposes parent AUTH eligibility for every source");
                check(snapshot && nlohmann::json::parse(snapshot_json(
                                      *snapshot))["targets"][3]["sealed_available"] == true,
                      "status JSON exposes sealed eligibility");
                const auto journal =
                    nlohmann::json::parse(read(root / "var/lib/smile2unlock/deployment.json"));
                check(journal["version"] == 3 &&
                          journal["targets"]["gdm"]["authtok_source"] == authtok_source_id(next),
                      "v3 journal records named source");
                check(check_package_upgrade(root, "999.0.0").has_value(),
                      "upgrade accepts each source");
            }
            // PAM rollback must preserve independently enrolled user data.
            const auto token = root / "var/lib/smile2unlock/users/1000/authtok.s2u";
            write(token, "encrypted-user-data-fixture");
            check(rollback_all_pam_integrations(root).has_value(), "all sources roll back");
            check(read(parent) == original && std::filesystem::exists(token),
                  "rollback restores parent and preserves user token");
        }
    }
    const auto root = base / "conflict";
    for (const auto layout :
         {"auth optional pam_gnome_keyring.so\nauth include system-auth\n",
          "auth include system-auth\nsession optional pam_kwallet5.so auto_start\n",
          "auth include system-auth\nauth optional pam_exec.so pam_gnome_keyring.so\n",
          "auth include system-auth\n"}) {
        write(root / "etc/pam.d/gdm-password", layout);
        const auto snapshot = inspect_deployment(root);
        check(snapshot && !snapshot->targets[3].sealed_available,
              "invalid parent AUTH order is unavailable in status");
        check(!plan_pam_integration(root, TargetKind::kGdm, AuthtokSource::kSealed),
              "wallet before anchor, session-only, or in password stack conflicts");
    }
    for (const auto target : {TargetKind::kKscreenlocker, TargetKind::kDms, TargetKind::kSudo,
                              TargetKind::kSudoLogin, TargetKind::kPolkit}) {
        write(root / "etc/pam.d" / target_service(target),
              "auth include system-auth\nauth optional pam_kwallet5.so\n");
        const auto snapshot = inspect_deployment(root);
        check(snapshot && std::ranges::none_of(snapshot->targets,
                                               [target](const auto& status) {
                                                   return status.kind == target &&
                                                          status.sealed_available;
                                               }),
              "lock/privilege status does not offer sealed");
        for (const auto source : {AuthtokSource::kSealed, AuthtokSource::kKernelKeyring})
            check(!plan_pam_integration(root, target, source),
                  "lock and privilege targets refuse wallet sources");
    }
    for (const auto mode : {0, 1, 2, 3}) {
        const auto kernel = mode == 1;
        const auto migration_root = base / std::format("migration-{}", mode);
        write(migration_root / "etc/pam.d/gdm-password",
              "auth include system-auth\nauth optional pam_kwallet5.so\n");
        auto plan = plan_pam_integration(migration_root, TargetKind::kGdm, kernel);
        check(plan && apply_pam_plan(migration_root, *plan), "migration fixture applies");
        const auto journal_path = migration_root / "var/lib/smile2unlock/deployment.json";
        auto journal = nlohmann::json::parse(read(journal_path));
        journal["version"] = mode == 3 ? 1 : 2;
        journal["transformation_version"] = 2;
        journal["targets"]["gdm"]["transformation_version"] = 2;
        journal["targets"]["gdm"].erase("authtok_source");
        if (mode < 2)
            journal["targets"]["gdm"]["wallet_token"] = kernel;
        write(journal_path, journal.dump());
        plan = plan_pam_integration(migration_root, TargetKind::kGdm, AuthtokSource::kSealed);
        check(plan && apply_pam_plan(migration_root, *plan),
              "v2 journal upgrades directly to sealed");
        check(nlohmann::json::parse(read(journal_path))["version"] == 3, "migration persists v3");
        check(rollback_pam_integration(migration_root, TargetKind::kGdm).has_value(),
              "migrated target rolls back");
    }
    for (const auto initial :
         {AuthtokSource::kNone, AuthtokSource::kKernelKeyring, AuthtokSource::kSealed}) {
        for (const auto next :
             {AuthtokSource::kNone, AuthtokSource::kKernelKeyring, AuthtokSource::kSealed}) {
            if (initial == next)
                continue;
            const auto recovery_root =
                base /
                std::format("recover-{}-{}", authtok_source_id(initial), authtok_source_id(next));
            write(recovery_root / "etc/pam.d/gdm-password",
                  "auth include system-auth\nauth optional pam_kwallet5.so\n");
            const auto old_plan = plan_pam_integration(recovery_root, TargetKind::kGdm, initial);
            check(old_plan && apply_pam_plan(recovery_root, *old_plan), "recovery fixture applies");
            const auto next_plan = plan_pam_integration(recovery_root, TargetKind::kGdm, next);
            check(next_plan.has_value(), "interrupted source switch plans");
            if (!old_plan || !next_plan)
                continue;
            const auto journal_path = recovery_root / "var/lib/smile2unlock/deployment.json";
            auto journal = nlohmann::json::parse(read(journal_path));
            const auto backup =
                recovery_root / "var/lib/smile2unlock/pam-backups/gdm.upgrade-child";
            write(backup, old_plan->child_content);
            journal["targets"]["gdm"]["state"] = "upgrading";
            journal["targets"]["gdm"]["upgrade_child_backup"] = backup.string();
            journal["targets"]["gdm"]["pending_child_result_fingerprint"] =
                fingerprint(next_plan->child_content);
            write(journal_path, journal.dump());
            write(next_plan->child_path, next_plan->child_content);
            check(recover_interrupted_pam_transactions(recovery_root).has_value(),
                  "interrupted source switch recovers");
            check(read(old_plan->child_path) == old_plan->child_content,
                  "recovery restores prior source layout");
            const auto recovered = nlohmann::json::parse(read(journal_path));
            check(recovered["targets"]["gdm"]["authtok_source"] == authtok_source_id(initial),
                  "recovery preserves prior source metadata");
            check(rollback_all_pam_integrations(recovery_root).has_value(),
                  "recovered source rolls back");
        }
    }
    return passed;
}
