#include "platform/linux/deploy/deployment.h"

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

    std::filesystem::remove_all(root);
    if (failures != 0) {
        std::cerr << failures << " deployment checks failed\n";
        return 1;
    }
    std::cout << "deployment checks passed\n";
    return 0;
}
