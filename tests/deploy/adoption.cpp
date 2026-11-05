#include "platform/linux/deploy/deployment.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, std::string_view message) {
    if (!ok) {
        std::cerr << "FAIL adoption: " << message << '\n';
        ++failures;
    }
}
void write(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}
std::string read(const std::filesystem::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
constexpr auto prefix = "# Legacy setup\r\nauth       required     pam_securetty.so\n"
                        "auth       requisite    pam_nologin.so\n";
constexpr auto anchor = "auth       include      system-local-login\n";
constexpr auto tail = "auth       optional     pam_gnome_keyring.so\n"
                      "account include system-local-login\n# no final newline";
} // namespace

bool adoption_deployment_tests(const std::filesystem::path& base) {
    using namespace su::deploy;
    for (const auto source :
         {AuthtokSource::kNone, AuthtokSource::kSealed, AuthtokSource::kKernelKeyring}) {
        for (const auto vendor : {false, true}) {
            const auto root =
                base / (std::string(authtok_source_id(source)) + (vendor ? "-vendor" : ""));
            const auto path = root / (vendor ? "usr/lib/pam.d/greetd" : "etc/pam.d/greetd");
            const auto original = std::string(prefix) + "auth sufficient pam_smile2unlock.so" +
                                  (vendor ? " socket=/run/smile2unlock/control.sock\n" : "\n") +
                                  anchor + tail;
            write(path, original);
            const auto snapshot = inspect_deployment(root);
            check(snapshot && snapshot->targets[5].state == TargetState::kExternal &&
                      snapshot->targets[5].adoptable &&
                      snapshot->targets[5].adoption_unavailable_reason.empty(),
                  "legacy greetd is adoptable");
            if (snapshot) {
                const auto status = nlohmann::json::parse(snapshot_json(*snapshot))["targets"][5];
                check(status["adoptable"] == true && status["adoption_unavailable_reason"] == "",
                      "status protocol exposes adoption capability");
            }
            check(!plan_pam_integration(root, TargetKind::kGreetd, source),
                  "normal configure continues to refuse external setup");
            const auto plan = plan_pam_adoption(root, TargetKind::kGreetd, source);
            check(plan.has_value(), "adoption plan generated");
            if (!plan)
                continue;
            check(plan->source_content == original && plan->adoption,
                  "plan keeps exact original bytes");
            const auto review = nlohmann::json::parse(plan_json(*plan));
            check(review["operation"] == "adopt" && review["replaces_manual_line"] == true &&
                      review["child_content"] == plan->child_content,
                  "review describes adoption and generated substack");
            check(!review.contains("original_content") && !review.contains("destination_content"),
                  "unprivileged review does not disclose original PAM options");
            // Removing the single legacy line must produce precisely the normal configure result.
            const auto clean_root = root / "clean";
            write(clean_root / "etc/pam.d/greetd", std::string(prefix) + anchor + tail);
            const auto clean = plan_pam_integration(clean_root, TargetKind::kGreetd, source);
            check(clean && clean->destination_content == plan->destination_content &&
                      clean->child_content == plan->child_content,
                  "adoption equals normal transformation");
            check(plan->destination_content.find("pam_smile2unlock.so") == std::string::npos &&
                      plan->destination_content.find("smile2unlock-greetd-auth") <
                          plan->destination_content.find("pam_gnome_keyring.so"),
                  "wallet follows managed anchor");
            check(source != AuthtokSource::kSealed ||
                      plan->child_content.find("authtok=sealed") != std::string::npos,
                  "sealed source propagated");
            check(apply_pam_plan(root, *plan).has_value(), "adoption applies transactionally");
            const auto journal = nlohmann::json::parse(
                read(root / "var/lib/smile2unlock/deployment.json"));
            check(read(journal["targets"]["greetd"]["parent_backup"].get<std::string>()) == original,
                  "journal records exact original bytes for administrator and vendor files");
            check(read(plan->destination_path) == plan->destination_content &&
                      read(plan->child_path) == plan->child_content,
                  "managed files installed");
            const auto managed = inspect_deployment(root);
            check(managed && managed->targets[5].state == TargetState::kManaged &&
                      managed->targets[5].password_fallback && !managed->targets[5].adoptable,
                  "adopted target is managed with fallback");
            const auto modified = plan->destination_content + "# administrator edit\n";
            write(plan->destination_path, modified);
            check(!rollback_pam_integration(root, TargetKind::kGreetd),
                  "rollback refuses parent edits");
            check(read(plan->destination_path) == modified, "refused rollback preserves edits");
            write(plan->destination_path, plan->destination_content);
            check(rollback_pam_integration(root, TargetKind::kGreetd).has_value(),
                  "adoption rollback succeeds");
            check(read(path) == original,
                  "rollback restores legacy bytes including CR and missing final newline");
            check(!vendor || !std::filesystem::exists(plan->destination_path),
                  "vendor override removed");
            check(!std::filesystem::exists(plan->child_path), "rollback removes created substack");
        }
    }
    const auto rejected = std::vector<std::string>{
        "auth sufficient pam_smile2unlock.so debug\nauth include system-login\n",
        "auth sufficient pam_smile2unlock.so socket=/tmp/other.sock\nauth include system-login\n",
        "auth sufficient pam_smile2unlock.so\nauth sufficient pam_smile2unlock.so\nauth include "
        "system-login\n",
        "auth required pam_smile2unlock.so\nauth include system-login\n",
        "auth include system-login\nauth sufficient pam_smile2unlock.so\n",
        "auth sufficient pam_smile2unlock.so\nauth optional pam_env.so\nauth include "
        "system-login\n",
        "auth sufficient pam_smile2unlock.so\nauth include custom-stack\n",
        "auth sufficient /usr/lib/security/pam_smile2unlock.so\nauth include system-login\n",
        "auth sufficient pam_smile2unlock.so\nauth include system-login\nauth include "
        "smile2unlock-other-auth\n",
        "@include smile2unlock-other-auth\nauth sufficient pam_smile2unlock.so\nauth include "
        "system-login\n",
        "auth sufficient pam_smile2unlock.so\nauth include system-login\nsession substack "
        "smile2unlock-other\n",
        "auth sufficient pam_smile2unlock.so\nauth include system-login\nauth substack "
        "/etc/pam.d/smile2unlock-other\n",
    };
    for (auto i = std::size_t{}; i < rejected.size(); ++i) {
        const auto root = base / ("refuse-" + std::to_string(i));
        write(root / "etc/pam.d/greetd", rejected[i]);
        const auto snapshot = inspect_deployment(root);
        check(snapshot && snapshot->targets[5].state == TargetState::kExternal &&
                  !snapshot->targets[5].adoptable &&
                  !snapshot->targets[5].adoption_unavailable_reason.empty(),
              "unsupported external layout reports reason");
        const auto plan = plan_pam_adoption(root, TargetKind::kGreetd);
        check(!plan && !plan.error().empty(), "unsafe adoption refused with reason");
        check(read(root / "etc/pam.d/greetd") == rejected[i] &&
                  !std::filesystem::exists(root / "var/lib/smile2unlock/deployment.json"),
              "refusal is read only");
    }
    const auto root = base / "stale";
    const auto path = root / "etc/pam.d/greetd";
    const auto original = std::string("auth sufficient pam_smile2unlock.so\n") + anchor + tail;
    write(path, original);
    const auto plan = plan_pam_adoption(root, TargetKind::kGreetd);
    check(plan && plan->authtok_source == AuthtokSource::kNone, "default source is none");
    write(path, original + "\n# changed");
    if (plan) {
        const auto result = apply_pam_plan(root, *plan);
        check(!result && result.error().find("changed") != std::string::npos,
              "stale adoption refused with reason");
        check(!std::filesystem::exists(plan->child_path) &&
                  !std::filesystem::exists(root / "var/lib/smile2unlock/deployment.json"),
              "stale plan writes nothing");
    }
    for (const auto target :
         {TargetKind::kKscreenlocker, TargetKind::kPlasmaLogin, TargetKind::kGdm, TargetKind::kSddm,
          TargetKind::kGreetd, TargetKind::kSudo, TargetKind::kSudoLogin, TargetKind::kPolkit}) {
        const auto target_root = base / std::string(target_id(target));
        write(target_root / "etc/pam.d" / target_service(target), original);
        check(plan_pam_adoption(target_root, target).has_value(), "supported target can adopt");
    }
    check(!plan_pam_adoption(root, TargetKind::kDms), "DMS remains on its dedicated installer");
    const auto no_wallet_root = base / "no-wallet";
    write(no_wallet_root / "etc/pam.d/greetd",
          "auth sufficient pam_smile2unlock.so\nauth include system-login\n");
    check(!plan_pam_adoption(no_wallet_root, TargetKind::kGreetd, AuthtokSource::kSealed),
          "sealed adoption requires a parent wallet hook");
    check(!plan_pam_adoption(base / "sudo", TargetKind::kSudo, AuthtokSource::kSealed),
          "privilege targets refuse sealed tokens");

    const auto unsafe_root = base / "symlink";
    write(unsafe_root / "original", original);
    std::filesystem::create_directories(unsafe_root / "etc/pam.d");
    std::filesystem::create_symlink(unsafe_root / "original", unsafe_root / "etc/pam.d/greetd");
    const auto unsafe = plan_pam_adoption(unsafe_root, TargetKind::kGreetd);
    check(!unsafe && unsafe.error().find("regular") != std::string::npos, "symlink target refused");

    const auto recovery_root = base / "recovery";
    const auto recovery_path = recovery_root / "etc/pam.d/greetd";
    write(recovery_path, original);
    const auto recovery_plan = plan_pam_adoption(recovery_root, TargetKind::kGreetd);
    check(recovery_plan && apply_pam_plan(recovery_root, *recovery_plan).has_value(),
          "recovery fixture adopted");
    if (recovery_plan) {
        const auto journal_path = recovery_root / "var/lib/smile2unlock/deployment.json";
        auto journal = nlohmann::json::parse(read(journal_path));
        const auto backup_path = journal["targets"]["greetd"]["parent_backup"].get<std::string>();
        check(read(backup_path) == original, "journal references exact original-byte backup");
        journal["targets"]["greetd"]["state"] = "applying";
        write(journal_path, journal.dump());
        // Simulate interruption after the child was installed but before the parent rename.
        write(recovery_path, original);
        check(recover_interrupted_pam_transactions(recovery_root).has_value(),
              "interrupted adoption recovers");
        check(read(recovery_path) == original &&
                  !std::filesystem::exists(recovery_plan->child_path),
              "recovery restores handwritten configuration exactly");
    }
    return failures == 0;
}
