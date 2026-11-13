#include "platform/linux/deploy/deployment.h"
#include "platform/linux/deploy/noctalia.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace su::deploy;

int failures = 0;

void check(bool ok, std::string_view message) {
    if (!ok) {
        std::cerr << "FAIL noctalia: " << message << '\n';
        ++failures;
    }
}

void write(const std::filesystem::path& path, std::string_view content, mode_t mode = 0644) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
    ::chmod(path.c_str(), mode);
}

std::string read(const std::filesystem::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

constexpr auto kPackagedService =
    "#%PAM-1.0\nauth       include      login\naccount    include      login\n"
    "password   include      login\nsession    include      login\n";

// A stand-in for the compiled helper: binary noise with C strings in it.
std::string fake_helper(std::string_view embedded_service) {
    auto bytes = std::string("\x7f" "ELF\0\0pam_start\0", 16);
    bytes += embedded_service;
    bytes.push_back('\0');
    bytes += "noctalia-pam-helper\0";
    return bytes;
}

const std::vector<std::string> kCandidates{"noctalia", "sudo", "sshd"};

void scanner_cases() {
    check(scan_compiled_pam_service(fake_helper("noctalia"), kCandidates).service == "noctalia",
          "NUL-delimited service is found");
    check(scan_compiled_pam_service("noctalia", kCandidates).outcome == NoctaliaScan::kService,
          "a name filling the whole buffer matches");
    check(scan_compiled_pam_service(std::string("\0noctalia", 9), kCandidates).outcome
              == NoctaliaScan::kService,
          "a name at the end of the buffer matches");
    check(scan_compiled_pam_service("xnoctalia-pam-helper\0", kCandidates).outcome
              == NoctaliaScan::kLogin,
          "a substring of a longer string does not match");
    check(scan_compiled_pam_service("", kCandidates).outcome == NoctaliaScan::kLogin,
          "an empty binary means the login default");
    const auto login = scan_compiled_pam_service(fake_helper("login"), kCandidates);
    check(login.outcome == NoctaliaScan::kLogin && login.service == "login",
          "the default build reports login");
    check(scan_compiled_pam_service(fake_helper("noctalia") + std::string("sudo\0", 5), kCandidates)
                  .service == "noctalia",
          "unrelated matches never displace the Noctalia-branded name");
    check(scan_compiled_pam_service(std::string("sudo\0sshd\0", 10), kCandidates).outcome
              == NoctaliaScan::kLogin,
          "several unrelated matches are ignored, not guessed from");
    const auto lone = scan_compiled_pam_service(std::string("sudo\0", 5), kCandidates);
    check(lone.outcome == NoctaliaScan::kLogin && lone.service == "login",
          "a single non-Noctalia match is no longer accepted");
    const std::vector<std::string> two_branded{"noctalia", "noctalia-lock"};
    check(scan_compiled_pam_service(std::string("noctalia\0noctalia-lock\0", 25), two_branded)
                  .outcome == NoctaliaScan::kAmbiguous,
          "several Noctalia-branded matches are refused");
    check(is_valid_pam_service_name("noctalia-lock.1") && !is_valid_pam_service_name("a/b")
              && !is_valid_pam_service_name("") && !is_valid_pam_service_name(".hidden")
              && !is_valid_pam_service_name(std::string(65, 'a')),
          "service names are plain file names");
}

// Tests run unprivileged, so the "root" lookup trusts the test user as owner;
// the owner check itself is exercised in noctalia_hardening.cpp.
const HelperLookup& test_root_lookup() {
    static const HelperLookup lookup = RootHelperLookup{::geteuid()};
    return lookup;
}

struct EnvGuard {
    EnvGuard(const char* helper, const char* path) {
        set("NOCTALIA_PAM_HELPER_PATH", helper);
        set("PATH", path);
    }
    ~EnvGuard() { ::setenv("PATH", "/usr/bin:/bin", 1); ::unsetenv("NOCTALIA_PAM_HELPER_PATH"); }
    static void set(const char* name, const char* value) {
        value != nullptr ? (void)::setenv(name, value, 1) : (void)::unsetenv(name);
    }
};

TargetStatus noctalia_status(const std::filesystem::path& root,
                             const HelperLookup& lookup = noctalia_search_from_environment()) {
    const auto snapshot = inspect_deployment(root, lookup);
    check(snapshot.has_value(), "snapshot is generated");
    const auto found = std::ranges::find(snapshot->targets, TargetKind::kNoctalia,
                                          &TargetStatus::kind);
    check(found != snapshot->targets.end(), "snapshot lists the Noctalia target");
    return found == snapshot->targets.end() ? TargetStatus{} : *found;
}

// An unprivileged account with a password, in a throwaway passwd/shadow.
PlanContext test_context(const std::filesystem::path& root) {
    write(root / "etc/passwd", "tester:x:" + std::to_string(::geteuid()) + ":100::/home/t:/bin/sh\n");
    write(root / "etc/shadow", "tester:$6$hash:19000:0:99999:7:::\n", 0600);
    return {.helper = test_root_lookup(), .requester_uid = ::geteuid()};
}

void detection_states(const std::filesystem::path& base) {
    const auto empty_bin = base / "empty-bin";
    std::filesystem::create_directories(empty_bin);
    {
        const auto root = base / "absent";
        write(root / "etc/pam.d/noctalia", kPackagedService);
        const EnvGuard env(nullptr, empty_bin.c_str());
        check(noctalia_status(root).state == TargetState::kAbsent, "no Noctalia means absent");
    }
    {
        const auto root = base / "old";
        const auto bin = root / "usr/bin";
        write(bin / "noctalia", "#!/bin/sh\n", 0755);
        const EnvGuard env(nullptr, bin.c_str());
        const auto status = noctalia_status(root);
        check(status.state == TargetState::kUnsupported
                  && status.note == "target.noctalia_needs_newer",
              "a shell without the PAM helper needs a newer Noctalia");
    }
    {
        const auto root = base / "login";
        const auto bin = root / "usr/bin";
        write(bin / "noctalia", "#!/bin/sh\n", 0755);
        write(root / "usr/libexec/noctalia/noctalia-pam-helper", fake_helper("login"), 0755);
        write(root / "etc/pam.d/login", "auth required pam_unix.so\n");
        write(root / "etc/pam.d/noctalia", kPackagedService);
        const EnvGuard env(nullptr, bin.c_str());
        const auto status = noctalia_status(root);
        check(status.state == TargetState::kUnsupported
                  && status.note == "target.noctalia_built_with_login",
              "a login build is unsupported");
        check(!plan_pam_integration(root, TargetKind::kNoctalia, false, test_context(root)),
              "a login build is never planned");
        check(read(root / "etc/pam.d/login") == "auth required pam_unix.so\n",
              "login is untouched");
    }
    {
        const auto root = base / "missing-file";
        const auto helper = root / "opt/noctalia-pam-helper";
        write(helper, fake_helper("noctalia"), 0755);
        write(root / "etc/pam.d/other-service", kPackagedService);
        const EnvGuard env(helper.c_str(), empty_bin.c_str());
        // The scanner only considers names that exist in /etc/pam.d.
        check(noctalia_status(root).note == "target.noctalia_built_with_login",
              "a service without a package file reads as the login default");
    }
}

void managed_round_trip(const std::filesystem::path& root) {
    const auto helper = root / "usr/local/libexec/noctalia/noctalia-pam-helper";
    write(helper, fake_helper("noctalia"), 0755);
    write(root / "etc/pam.d/noctalia", kPackagedService);
    write(root / "etc/pam.d/login", "auth required pam_unix.so\n");
    const EnvGuard env(nullptr, "/nonexistent");

    const auto before = noctalia_status(root, test_root_lookup());
    check(before.state == TargetState::kSupported && before.service == "noctalia"
              && before.note == "target.noctalia_press_enter",
          "a Noctalia build with a packaged service is configurable");
    const auto plan = plan_pam_integration(root, TargetKind::kNoctalia, false, test_context(root));
    check(plan.has_value() && plan->service == "noctalia", "the detected service is planned");
    if (!plan) {
        return;
    }
    check(apply_pam_plan(root, *plan).has_value(), "the plan applies");
    const auto parent = read(root / "etc/pam.d/noctalia");
    check(parent.contains("substack") && parent.contains("smile2unlock-noctalia-auth"),
          "the packaged service delegates to the managed substack");
    check(read(root / "etc/pam.d/smile2unlock-noctalia-auth").contains("pam_smile2unlock.so"),
          "the managed substack carries the face module");
    check(read(root / "etc/pam.d/login") == "auth required pam_unix.so\n", "login is untouched");
    const auto managed = noctalia_status(root, test_root_lookup());
    check(managed.state == TargetState::kManaged && managed.password_fallback,
          "the target reports managed with a password fallback");
    check(rollback_pam_integration(root, TargetKind::kNoctalia).has_value(),
          "rollback succeeds");
    check(read(root / "etc/pam.d/noctalia") == kPackagedService
              && !std::filesystem::exists(root / "etc/pam.d/smile2unlock-noctalia-auth"),
          "rollback restores the packaged service");
    check(noctalia_status(root, test_root_lookup()).state == TargetState::kSupported,
          "the target is configurable again");
}

} // namespace

bool noctalia_hardening_tests(const std::filesystem::path& base);

bool noctalia_deployment_tests(const std::filesystem::path& base) {
    const auto before = failures;
    scanner_cases();
    detection_states(base / "detection");
    managed_round_trip(base / "managed");
    return noctalia_hardening_tests(base / "hardening") && failures == before;
}
