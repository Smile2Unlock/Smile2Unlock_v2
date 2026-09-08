#include "platform/linux/deploy/deployment.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void write_fixture(
    const std::filesystem::path& root,
    std::string_view service,
    std::string_view content,
    std::string_view directory = "usr/lib/pam.d") {
    const auto path = root / directory / service;
    std::filesystem::create_directories(path.parent_path());
    auto output = std::ofstream(path, std::ios::binary);
    output << content;
}

std::string read_file(const std::filesystem::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void arch_plasma_round_trip(const std::filesystem::path& root) {
    constexpr auto fixture =
        "#%PAM-1.0\n"
        "auth include system-login\n"
        "-auth optional pam_gnome_keyring.so\n"
        "-auth optional pam_kwallet5.so\n"
        "account include system-login\n"
        "session include system-login\n";
    write_fixture(root, "plasmalogin", fixture);

    const auto plan = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kPlasmaLogin, true);
    check(plan.has_value(), "Arch Plasma Login plan is generated");
    if (!plan) {
        return;
    }
    check(plan->destination_content.find("auth       substack    smile2unlock-plasma-login-auth")
              != std::string::npos,
          "parent service uses a dedicated substack");
    check(plan->child_content.find("pam_systemd_loadkey.so") != std::string::npos,
          "wallet token is restricted to the face substack");
    check(plan->destination_content.find("pam_kwallet5.so") != std::string::npos,
          "wallet module remains in the parent stack");

    const auto applied = su::deploy::apply_pam_plan(root, *plan);
    check(applied.has_value(), "PAM plan applies in an isolated root");
    if (!applied) {
        return;
    }
    const auto snapshot = su::deploy::inspect_deployment(root);
    check(snapshot.has_value(), "deployment snapshot loads after apply");
    if (snapshot) {
        const auto& status = snapshot->targets[2];
        check(status.state == su::deploy::TargetState::kManaged,
              "Plasma Login is reported as managed");
        check(status.password_fallback, "managed substack retains password fallback");
        check(status.wallet_token_enabled, "managed substack reports wallet token support");
    }

    const auto rolled_back = su::deploy::rollback_pam_integration(
        root, su::deploy::TargetKind::kPlasmaLogin);
    check(rolled_back.has_value(), "managed PAM integration rolls back");
    check(!std::filesystem::exists(root / "etc/pam.d/plasmalogin"),
          "vendor-backed administrator override is removed on rollback");
    check(read_file(root / "usr/lib/pam.d/plasmalogin") == fixture,
          "vendor PAM service remains unchanged");
}

void kscreenlocker_round_trip(const std::filesystem::path& root) {
    constexpr auto fixture =
        "#%PAM-1.0\n"
        "auth include system-local-login\n"
        "account include system-local-login\n"
        "session include system-local-login\n";
    write_fixture(root, "kde", fixture);

    const auto plan = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kKscreenlocker, false);
    check(plan.has_value(), "KScreenLocker plan is generated");
    if (!plan) {
        return;
    }
    check(plan->child_content.find("auth sufficient pam_smile2unlock.so")
              != std::string::npos,
          "KScreenLocker substack attempts face authentication first");
    check(plan->child_content.find("auth substack system-local-login")
              != std::string::npos,
          "KScreenLocker substack retains password fallback");
    check(plan->destination_content.find("account include system-local-login")
              != std::string::npos,
          "KScreenLocker account policy remains in the parent stack");

    check(su::deploy::apply_pam_plan(root, *plan).has_value(),
          "KScreenLocker plan applies in an isolated root");
    check(su::deploy::rollback_pam_integration(
              root, su::deploy::TargetKind::kKscreenlocker).has_value(),
          "KScreenLocker plan rolls back");
    check(read_file(root / "usr/lib/pam.d/kde") == fixture,
          "KScreenLocker vendor service remains unchanged");
}

void fedora_gdm_preserves_policy(const std::filesystem::path& root) {
    constexpr auto fixture =
        "auth [success=done ignore=ignore default=bad] pam_selinux_permit.so\n"
        "auth substack password-auth\n"
        "auth optional pam_gnome_keyring.so\n"
        "auth include postlogin\n"
        "account required pam_nologin.so\n"
        "account include password-auth\n"
        "session required pam_selinux.so open\n";
    write_fixture(root, "gdm-password", fixture, "etc/pam.d");
    const auto plan = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kGdm, false);
    check(plan.has_value(), "Fedora GDM plan is generated");
    if (!plan) {
        return;
    }
    check(plan->destination_content.starts_with(
              "auth [success=done ignore=ignore default=bad] pam_selinux_permit.so"),
          "SELinux permit remains before the authentication substack");
    check(plan->destination_content.find("auth include postlogin") != std::string::npos,
          "Fedora postlogin remains in the parent service");
    check(plan->child_content.find("pam_systemd_loadkey.so") == std::string::npos,
          "wallet token branch is omitted unless explicitly requested");
}

void debian_include_is_supported(const std::filesystem::path& root) {
    constexpr auto fixture =
        "auth requisite pam_nologin.so\n"
        "auth required pam_succeed_if.so user != root quiet_success\n"
        "@include common-auth\n"
        "-auth optional pam_gnome_keyring.so\n"
        "@include common-account\n"
        "@include common-session\n";
    write_fixture(root, "gdm-password", fixture);
    const auto plan = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kGdm, false);
    check(plan.has_value(), "Debian @include common-auth is recognized");
    if (plan) {
        check(plan->child_content.find("auth substack common-auth") != std::string::npos,
              "Debian password include becomes a bounded child substack");
        check(plan->destination_content.find("pam_succeed_if.so user != root")
                  != std::string::npos,
              "Debian root policy remains before face authentication");
    }
}

void sddm_round_trip(const std::filesystem::path& root) {
    constexpr auto interactive_fixture =
        "#%PAM-1.0\n"
        "auth include system-login\n"
        "account include system-login\n"
        "session include system-login\n";
    constexpr auto autologin_fixture =
        "#%PAM-1.0\n"
        "auth required pam_permit.so\n"
        "account include system-login\n"
        "session include system-login\n";
    write_fixture(root, "sddm", interactive_fixture);
    write_fixture(root, "sddm-autologin", autologin_fixture);

    const auto plan = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kSddm, false);
    check(plan.has_value(), "SDDM interactive-login plan is generated");
    if (!plan) {
        return;
    }
    check(plan->service == "sddm", "SDDM plan targets only the interactive service");
    check(plan->child_content.find("auth substack system-login") != std::string::npos,
          "SDDM plan retains the system password stack");
    check(su::deploy::apply_pam_plan(root, *plan).has_value(),
          "SDDM interactive-login plan applies");
    check(read_file(root / "usr/lib/pam.d/sddm-autologin") == autologin_fixture,
          "SDDM autologin service remains unchanged");
    check(su::deploy::rollback_pam_integration(
              root, su::deploy::TargetKind::kSddm).has_value(),
          "SDDM interactive-login plan rolls back");
}

void dms_password_fallback_is_detected(const std::filesystem::path& root) {
    constexpr auto fixture =
        "#%PAM-1.0\n"
        "auth sufficient pam_smile2unlock.so socket=/run/smile2unlock/control.sock\n"
        "auth include login\n"
        "account include login\n"
        "session include login\n";
    write_fixture(root, "dankshell-smile2unlock", fixture, "etc/pam.d");
    const auto snapshot = su::deploy::inspect_deployment(root);
    check(snapshot.has_value(), "DMS deployment snapshot is generated");
    if (snapshot) {
        check(snapshot->targets.front().state == su::deploy::TargetState::kManaged,
              "DMS service is reported as managed");
        check(snapshot->targets.front().password_fallback,
              "DMS login include is reported as password fallback");
    }
}

void external_and_stale_changes_are_refused(const std::filesystem::path& root) {
    write_fixture(
        root,
        "kde",
        "auth sufficient pam_smile2unlock.so\nauth include system-local-login\n");
    const auto external = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kKscreenlocker, false);
    check(!external, "externally configured PAM service is not overwritten");

    write_fixture(root, "sddm", "auth include system-local-login\n");
    const auto plan = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kSddm, false);
    check(plan.has_value(), "SDDM plan is generated");
    if (!plan) {
        return;
    }
    write_fixture(root, "sddm", "auth include system-local-login\n# external edit\n");
    const auto applied = su::deploy::apply_pam_plan(root, *plan);
    check(!applied, "stale PAM plan is rejected");
}

void managed_upgrade_and_downgrade_guard(const std::filesystem::path& root) {
    constexpr auto fixture = "auth include system-login\naccount include system-login\n";
    write_fixture(root, "plasmalogin", fixture);
    const auto initial = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kPlasmaLogin, false);
    check(initial && su::deploy::apply_pam_plan(root, *initial).has_value(),
          "initial managed target applies before upgrade");
    const auto upgrade = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kPlasmaLogin, true);
    check(upgrade && !upgrade->already_managed,
          "wallet policy change produces a managed upgrade plan");
    check(upgrade && su::deploy::apply_pam_plan(root, *upgrade).has_value(),
          "managed PAM target upgrades transactionally");
    check(read_file(root / "etc/pam.d/smile2unlock-plasma-login-auth")
              .find("pam_systemd_loadkey.so") != std::string::npos,
          "upgraded child stack contains the requested wallet branch");

    const auto journal_path = root / "var/lib/smile2unlock/deployment.json";
    auto journal = nlohmann::json::parse(read_file(journal_path));
    journal["package_version"] = "99.0.0";
    {
        auto output = std::ofstream(journal_path, std::ios::binary | std::ios::trunc);
        output << journal.dump(2) << '\n';
    }
    const auto downgrade = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kPlasmaLogin, false);
    check(!downgrade, "an older package cannot replace a newer managed deployment");
}

void interrupted_transaction_recovers(const std::filesystem::path& root) {
    constexpr auto fixture = "auth include system-local-login\n";
    write_fixture(root, "kde", fixture);
    const auto plan = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kKscreenlocker, false);
    check(plan && su::deploy::apply_pam_plan(root, *plan).has_value(),
          "fixture applies before interrupted rollback simulation");
    const auto journal_path = root / "var/lib/smile2unlock/deployment.json";
    auto journal = nlohmann::json::parse(read_file(journal_path));
    journal["targets"]["kscreenlocker"]["state"] = "rolling_back";
    {
        auto output = std::ofstream(journal_path, std::ios::binary | std::ios::trunc);
        output << journal.dump(2) << '\n';
    }
    std::filesystem::remove(root / "etc/pam.d/kde");
    check(su::deploy::recover_interrupted_pam_transactions(root).has_value(),
          "interrupted two-file transaction is recovered on startup");
    check(!std::filesystem::exists(root / "etc/pam.d/smile2unlock-kscreenlocker-auth"),
          "recovery removes the remaining managed child");
}

void interrupted_upgrade_rejects_corrupt_backup(const std::filesystem::path& root) {
    constexpr auto fixture = "auth include system-login\naccount include system-login\n";
    write_fixture(root, "plasmalogin", fixture);
    const auto initial = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kPlasmaLogin, false);
    check(initial && su::deploy::apply_pam_plan(root, *initial).has_value(),
          "fixture applies before interrupted upgrade simulation");
    if (!initial) {
        return;
    }

    const auto journal_path = root / "var/lib/smile2unlock/deployment.json";
    auto journal = nlohmann::json::parse(read_file(journal_path));
    journal["targets"]["plasma-login"]["state"] = "upgrading";
    journal["targets"]["plasma-login"]["pending_child_result_fingerprint"] = "pending";
    {
        auto output = std::ofstream(journal_path, std::ios::binary | std::ios::trunc);
        output << journal.dump(2) << '\n';
    }
    const auto backup = root
        / "var/lib/smile2unlock/backups/plasma-login.upgrade-child";
    std::filesystem::create_directories(backup.parent_path());
    {
        auto output = std::ofstream(backup, std::ios::binary);
        output << "corrupt backup\n";
    }
    check(!su::deploy::recover_interrupted_pam_transactions(root).has_value(),
          "interrupted upgrade rejects a backup with the wrong fingerprint");
    check(read_file(root / "etc/pam.d/smile2unlock-plasma-login-auth")
              == initial->child_content,
          "failed recovery leaves the managed PAM substack unchanged");
}

void rollback_all_is_idempotent(const std::filesystem::path& root) {
    write_fixture(root, "kde", "auth include system-local-login\n");
    write_fixture(root, "sddm", "auth include system-login\n");
    for (const auto target : {su::deploy::TargetKind::kKscreenlocker,
             su::deploy::TargetKind::kSddm}) {
        const auto plan = su::deploy::plan_pam_integration(root, target, false);
        check(plan && su::deploy::apply_pam_plan(root, *plan).has_value(),
              "target applies before bulk rollback");
    }
    check(su::deploy::rollback_all_pam_integrations(root).has_value(),
          "bulk rollback removes every managed target");
    check(su::deploy::rollback_all_pam_integrations(root).has_value(),
          "bulk rollback is idempotent for package uninstall");
}

void opensuse_common_stack_round_trip(const std::filesystem::path& root) {
    // openSUSE composes the distribution common-* stacks with postlogin in
    // both the login manager and the lock screen.
    constexpr auto fixture =
        "#%PAM-1.0\n"
        "auth     include        common-auth\n"
        "auth     include        postlogin\n"
        "account  include        common-account\n"
        "password include        common-password\n"
        "session  include        common-session\n"
        "session  include        postlogin\n";
    write_fixture(root, "plasmalogin", fixture);

    const auto plan = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kPlasmaLogin, false);
    check(plan.has_value(), "openSUSE Plasma Login common-* plan is generated");
    if (!plan) {
        return;
    }
    check(plan->child_content.find("auth substack common-auth") != std::string::npos,
          "openSUSE child stack keeps common-auth as password fallback");
    check(plan->destination_content.find("auth       substack    smile2unlock-plasma-login-auth")
              != std::string::npos,
          "openSUSE common-auth anchor becomes the managed substack");
    check(plan->destination_content.find("auth     include        postlogin")
              != std::string::npos,
          "openSUSE postlogin authentication policy stays in the parent service");
    check(plan->destination_content.find("session  include        postlogin")
              != std::string::npos,
          "openSUSE postlogin session policy stays in the parent service");
    check(su::deploy::apply_pam_plan(root, *plan).has_value(),
          "openSUSE common-* plan applies in an isolated root");
    check(su::deploy::rollback_pam_integration(
              root, su::deploy::TargetKind::kPlasmaLogin).has_value(),
          "openSUSE common-* plan rolls back");
}

void comments_and_control_expressions_are_preserved(const std::filesystem::path& root) {
    constexpr auto fixture =
        "#%PAM-1.0\n"
        "# Distribution default; do not edit by hand.\n"
        "auth\t[success=1 default=ignore]\tpam_succeed_if.so user != root quiet_success\n"
        "auth\tinclude\tsystem-login\n"
        "-auth\toptional\tpam_gnome_keyring.so\n"
        "\n"
        "account\tinclude\tsystem-login\n"
        "session\tinclude\tsystem-login\n";
    write_fixture(root, "kde", fixture);

    const auto plan = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kKscreenlocker, false);
    check(plan.has_value(), "tabs, comments and control expressions parse");
    if (!plan) {
        return;
    }
    check(plan->destination_content.find("# Distribution default; do not edit by hand.")
              != std::string::npos,
          "leading comment is preserved verbatim");
    check(plan->destination_content.find(
              "auth\t[success=1 default=ignore]\tpam_succeed_if.so user != root quiet_success")
              != std::string::npos,
          "bracketed control expression is preserved verbatim");
    check(plan->destination_content.find("-auth\toptional\tpam_gnome_keyring.so")
              != std::string::npos,
          "dash-prefixed optional module is preserved verbatim");
    check(plan->child_content.find("auth substack system-login") != std::string::npos,
          "tab-separated password include is still detected as the anchor");
}

void upstream_variant_layouts_are_supported(const std::filesystem::path& root) {
    // Newer Fedora/upstream layouts use `auth include password-auth` instead
    // of `substack`, keep pam_selinux_permit before it, and prefix optional
    // modules with `-`.
    constexpr auto fixture =
        "auth       required      pam_env.so\n"
        "auth       [success=done ignore=ignore default=bad] pam_selinux_permit.so\n"
        "auth       include       password-auth\n"
        "-auth      optional      pam_gnome_keyring.so\n"
        "account    include       password-auth\n"
        "password   include       password-auth\n"
        "session    include       password-auth\n"
        "session    include       postlogin\n";
    write_fixture(root, "gdm-password", fixture, "etc/pam.d");

    const auto plan = su::deploy::plan_pam_integration(
        root, su::deploy::TargetKind::kGdm, false);
    check(plan.has_value(), "upstream `include password-auth` layout is recognized");
    if (!plan) {
        return;
    }
    check(plan->child_content.find("auth substack password-auth") != std::string::npos,
          "password-auth include becomes the managed password fallback");
    check(plan->destination_content.find(
              "auth       [success=done ignore=ignore default=bad] pam_selinux_permit.so")
              != std::string::npos,
          "SELinux permit keeps its position ahead of the managed substack");
    check(plan->destination_content.find("-auth      optional      pam_gnome_keyring.so")
              != std::string::npos,
          "optional wallet module stays in the parent service");
    check(plan->destination_content.find("session    include       postlogin")
              != std::string::npos,
          "postlogin session policy stays in the parent service");
}

} // namespace

int main() {
    const auto root = std::filesystem::current_path() / "build" / "test-data"
        / ("deploy-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    arch_plasma_round_trip(root / "arch");
    kscreenlocker_round_trip(root / "kscreenlocker");
    fedora_gdm_preserves_policy(root / "fedora");
    debian_include_is_supported(root / "debian");
    sddm_round_trip(root / "sddm");
    dms_password_fallback_is_detected(root / "dms");
    external_and_stale_changes_are_refused(root / "safety");
    managed_upgrade_and_downgrade_guard(root / "upgrade");
    interrupted_transaction_recovers(root / "recovery");
    interrupted_upgrade_rejects_corrupt_backup(root / "upgrade-corrupt-backup");
    rollback_all_is_idempotent(root / "uninstall");
    opensuse_common_stack_round_trip(root / "opensuse");
    comments_and_control_expressions_are_preserved(root / "comments");
    upstream_variant_layouts_are_supported(root / "upstream-variants");

    std::filesystem::remove_all(root);
    if (failures != 0) {
        std::cerr << failures << " deployment checks failed\n";
        return 1;
    }
    std::cout << "deployment checks passed\n";
    return 0;
}
