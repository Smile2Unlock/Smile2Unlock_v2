#include "platform/linux/deploy/account.h"
#include "platform/linux/deploy/deployment.h"
#include "platform/linux/deploy/noctalia.h"
#include "platform/linux/deploy/safe_read.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace su::deploy;

int failures = 0;

void check(bool ok, std::string_view message) {
    if (!ok) {
        std::cerr << "FAIL noctalia hardening: " << message << '\n';
        ++failures;
    }
}

void write(const std::filesystem::path& path, std::string_view content, mode_t mode = 0644) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    ::chmod(path.c_str(), mode);
}

std::string read(const std::filesystem::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

constexpr auto kThin = "auth include login\naccount include login\n";
constexpr auto kHelperRelative = "usr/libexec/noctalia/noctalia-pam-helper";
const std::string kHelperBytes = std::string("ELF\0pam_start\0noctalia\0", 23);

ServicePolicy policy() { return noctalia_service_policy(); }

HelperLookup root_lookup(uid_t owner = ::geteuid()) { return RootHelperLookup{owner}; }

std::optional<ServiceRejection> verdict(std::string_view name, std::string_view content) {
    const auto result = check_noctalia_service(name, content, policy());
    return result ? std::nullopt : std::optional{result.error()};
}

// --- 1. predicate -----------------------------------------------------------

void predicate_name_branches() {
    const auto rejects = [](std::string_view name, ServiceRejection reason, std::string_view why) {
        const auto result = check_noctalia_service_name(name, policy());
        check(!result && result.error() == reason, why);
    };
    rejects("no/slash", ServiceRejection::kInvalidName, "an invalid name is rejected");
    rejects("", ServiceRejection::kInvalidName, "an empty name is rejected");
    rejects("kde", ServiceRejection::kDenied, "another target's service is denied (kde)");
    rejects("gdm-password", ServiceRejection::kDenied, "another target's service is denied (gdm)");
    rejects("dankshell-smile2unlock", ServiceRejection::kDenied, "the DMS service is denied");
    rejects("lightdm", ServiceRejection::kNotNoctaliaNamed, "an unrelated name is not Noctalia's");
    rejects("smile2unlock-noctalia-auth", ServiceRejection::kChildService,
            "our own substack is never a parent");
    check(check_noctalia_service_name("noctalia", policy()).has_value(), "noctalia is accepted");
    check(check_noctalia_service_name("noctalia-lock", policy()).has_value(),
          "a branded variant is accepted");
    auto extended = policy();
    extended.denied.emplace_back("noctalia-reserved");
    const auto reserved = check_noctalia_service_name("noctalia-reserved", extended);
    check(!reserved && reserved.error() == ServiceRejection::kDenied,
          "the deny list wins even over the brand check");
}

void predicate_deny_list() {
    constexpr std::array denied{"su", "su-l", "sudo", "sudo-i", "polkit-1", "sshd", "passwd",
                                "chsh", "chfn", "runuser", "runuser-l", "systemd-user",
                                "system-auth", "system-login", "system-local-login",
                                "common-auth", "login", "other"};
    for (const auto name : denied) {
        const auto result = check_noctalia_service(name, kThin, policy());
        check(!result && result.error() == ServiceRejection::kDenied,
              std::string("denied system service: ") + name);
    }
    for (const auto& target_service : policy().denied) {
        check(check_noctalia_service_name(target_service, policy()).error()
                  == ServiceRejection::kDenied,
              "target-table service is denied: " + target_service);
    }
    check(!std::ranges::contains(policy().denied, std::string{}) && policy().denied.size() == 9,
          "every other kTargets entry is in the policy");
}

void predicate_wrapper_shapes() {
    check(!verdict("noctalia", kThin), "auth include login is a thin wrapper");
    check(!verdict("noctalia", "#%PAM-1.0\nauth\tsubstack\tlogin\naccount include login\n"),
          "auth substack login is a thin wrapper");
    check(!verdict("noctalia", "AUTH INCLUDE login # trailing comment\n"),
          "types are case-insensitive and comments are ignored");
    check(!verdict("noctalia", "auth substack smile2unlock-noctalia-auth\n"),
          "our own managed substack keeps the file recognised");
    const auto not_thin = ServiceRejection::kNotThinWrapper;
    check(verdict("noctalia", "auth required pam_unix.so\n") == not_thin,
          "a module line is not a wrapper");
    check(verdict("noctalia", "auth required pam_permit.so\nauth include login\n") == not_thin,
          "an extra auth line is not a wrapper");
    check(verdict("noctalia", "auth include system-auth\n") == not_thin,
          "wrapping another stack is not a wrapper");
    check(verdict("noctalia", "auth include login extra\n") == not_thin,
          "extra fields are not a wrapper");
    check(verdict("noctalia", "@include common-auth\nauth include login\n") == not_thin,
          "@include is refused");
    check(verdict("noctalia", "account include login\n") == not_thin,
          "no auth phase is not a wrapper");
    check(verdict("noctalia", "-auth include login\n") == not_thin,
          "an optional auth line is not the plain wrapper");
    check(verdict("noctalia", "auth sufficient pam_unix.so\n") == not_thin,
          "a control keyword other than include or substack is refused");
    check(verdict("sudo", kThin) == ServiceRejection::kDenied,
          "the name checks run before the content check");
    check(!service_rejection_text(ServiceRejection::kNotThinWrapper).empty()
              && !service_rejection_text(ServiceRejection::kInvalidName).empty()
              && !service_rejection_text(ServiceRejection::kDenied).empty()
              && !service_rejection_text(ServiceRejection::kNotNoctaliaNamed).empty()
              && !service_rejection_text(ServiceRejection::kChildService).empty(),
          "every rejection has text");
}

// --- 2/3. trust modes, safe reading, cache ----------------------------------

struct EnvRestore {
    ~EnvRestore() {
        ::setenv("PATH", "/usr/bin:/bin", 1);
        ::unsetenv("NOCTALIA_PAM_HELPER_PATH");
    }
};

NoctaliaDetection detect(const std::filesystem::path& root, const HelperLookup& lookup) {
    return detect_noctalia(root, lookup, policy());
}

void root_mode_ignores_environment(const std::filesystem::path& base) {
    const auto root = base / "env-vs-root";
    const auto planted = root / "home/user/noctalia-pam-helper";
    write(planted, kHelperBytes, 0755);
    write(root / "home/user/bin/noctalia", "#!/bin/sh\n", 0755);
    write(root / "etc/pam.d/noctalia", kThin);
    const EnvRestore restore;
    ::setenv("NOCTALIA_PAM_HELPER_PATH", planted.c_str(), 1);
    ::setenv("PATH", (root / "home/user/bin").c_str(), 1);

    const auto unprivileged = detect(root, noctalia_search_from_environment());
    check(unprivileged.install == NoctaliaInstall::kHelperFound
              && unprivileged.scan.service == "noctalia",
          "the unprivileged lookup honours the environment");
    const auto privileged = detect(root, root_lookup());
    check(privileged.install == NoctaliaInstall::kAbsent,
          "the root lookup ignores NOCTALIA_PAM_HELPER_PATH and PATH");
    // The environment must not leak into the root lookup through the shell either.
    ::unsetenv("NOCTALIA_PAM_HELPER_PATH");
    write(root / "usr/bin/noctalia", "#!/bin/sh\n", 0755);
    check(detect(root, root_lookup()).install == NoctaliaInstall::kNoHelper,
          "the root lookup finds the shell only under the fixed prefixes");
}

void root_mode_trust_checks(const std::filesystem::path& base) {
    const auto root = base / "trust";
    const auto helper = root / kHelperRelative;
    write(root / "etc/pam.d/noctalia", kThin);
    write(helper, kHelperBytes, 0755);
    check(detect(root, root_lookup()).install == NoctaliaInstall::kHelperFound,
          "an owner-matching, non-writable helper is trusted");
    check(detect(root, root_lookup(::geteuid() + 1)).install == NoctaliaInstall::kUntrusted,
          "a helper with the wrong owner is untrusted");
    check(detect(root, noctalia_search_from_environment()).install
              == NoctaliaInstall::kHelperFound,
          "the unprivileged lookup does not demand root ownership");
    ::chmod(helper.c_str(), 0775);
    check(detect(root, root_lookup()).install == NoctaliaInstall::kUntrusted,
          "a group-writable helper is untrusted");
    ::chmod(helper.c_str(), 0757);
    check(detect(root, root_lookup()).install == NoctaliaInstall::kUntrusted,
          "a world-writable helper is untrusted");
    ::chmod(helper.c_str(), 0644);
    check(detect(root, root_lookup()).install == NoctaliaInstall::kAbsent,
          "a non-executable helper is skipped");
}

void helper_reading_rules(const std::filesystem::path& base) {
    const auto root = base / "reading";
    const auto helper = root / kHelperRelative;
    write(root / "etc/pam.d/noctalia", kThin);

    std::filesystem::create_directories(helper.parent_path());
    write(root / "elsewhere", kHelperBytes, 0755);
    std::filesystem::create_symlink(root / "elsewhere", helper);
    check(detect(root, root_lookup()).install == NoctaliaInstall::kUnreadable,
          "a symlinked helper is refused (O_NOFOLLOW)");
    std::filesystem::remove(helper);

    check(::mkfifo(helper.c_str(), 0755) == 0, "a FIFO can be planted");
    check(detect(root, root_lookup()).install == NoctaliaInstall::kUnreadable,
          "a FIFO neither hangs nor passes (O_NONBLOCK + S_ISREG)");
    std::filesystem::remove(helper);

    // Sparse, so it costs nothing on disk: only its size matters.
    const int fd = ::open(helper.c_str(), O_CREAT | O_WRONLY, 0755);
    constexpr off_t kOversize = 64 * 1024 * 1024 + 1;
    check(fd >= 0 && ::ftruncate(fd, kOversize) == 0, "an oversize helper can be created");
    if (fd >= 0) {
        ::close(fd);
    }
    check(detect(root, root_lookup()).install == NoctaliaInstall::kUnreadable,
          "a helper above the cap is refused");
}

void capped_reader(const std::filesystem::path& base) {
    const auto file = base / "capped/data";
    write(file, "0123456789");
    const auto opened = open_regular_nofollow(file);
    check(opened.has_value(), "a regular file opens");
    if (!opened) {
        return;
    }
    check(read_capped(*opened, 10).value_or("") == "0123456789", "exactly the cap is allowed");
    const auto over = read_capped(*opened, 9);
    check(!over && over.error() == ReadFailure::kTooLarge, "one byte over the cap fails");
    check(!open_regular_nofollow(base / "capped/missing")
              && open_regular_nofollow(base / "capped/missing").error() == OpenFailure::kMissing,
          "a missing file is reported as missing");
    check(!open_regular_nofollow(base / "capped")
              && open_regular_nofollow(base / "capped").error() == OpenFailure::kNotRegular,
          "a directory is not a regular file");
}

void detection_cache(const std::filesystem::path& base) {
    const auto root = base / "cache";
    const auto helper = root / kHelperRelative;
    write(root / "etc/pam.d/noctalia", kThin);
    write(helper, kHelperBytes, 0755);
    const auto start = noctalia_cache_stats();
    (void)detect(root, root_lookup());
    (void)detect(root, root_lookup());
    (void)detect(root, noctalia_search_from_environment());
    const auto warm = noctalia_cache_stats();
    check(warm.misses == start.misses + 1 && warm.hits >= start.hits + 2,
          "repeated detection scans the helper once");

    write(helper, kHelperBytes + std::string("padding"), 0755);
    (void)detect(root, root_lookup());
    check(noctalia_cache_stats().misses == warm.misses + 1, "a changed size rescans");

    const auto after_size = noctalia_cache_stats();
    const auto times = std::array{timespec{.tv_sec = 1'000'000, .tv_nsec = 0},
                                  timespec{.tv_sec = 1'000'000, .tv_nsec = 0}};
    ::utimensat(AT_FDCWD, helper.c_str(), times.data(), 0);
    (void)detect(root, root_lookup());
    check(noctalia_cache_stats().misses == after_size.misses + 1, "a changed mtime rescans");

    const auto after_mtime = noctalia_cache_stats();
    write(root / "replacement", kHelperBytes + std::string("padding"), 0755);
    ::utimensat(AT_FDCWD, (root / "replacement").c_str(), times.data(), 0);
    std::filesystem::rename(root / "replacement", helper);
    (void)detect(root, root_lookup());
    check(noctalia_cache_stats().misses == after_mtime.misses + 1,
          "a replaced inode rescans even with equal size and mtime");

    const auto settled = noctalia_cache_stats();
    ::chmod(helper.c_str(), 0775);
    check(detect(root, root_lookup()).install == NoctaliaInstall::kUntrusted
              && noctalia_cache_stats().misses == settled.misses,
          "the cache never bypasses the ownership and mode checks");
}

// --- 1. selection from pam.d, including vendor files ------------------------

void service_selection(const std::filesystem::path& base) {
    {
        const auto root = base / "not-wrapper";
        write(root / kHelperRelative, kHelperBytes, 0755);
        write(root / "etc/pam.d/noctalia", "auth required pam_unix.so\n");
        check(detect(root, root_lookup()).scan.outcome == NoctaliaScan::kLogin,
              "a service that is not a thin wrapper is not selected");
    }
    {
        const auto root = base / "no-fallback";
        write(root / kHelperRelative, std::string("pam\0sddm\0", 9), 0755);
        write(root / "etc/pam.d/sddm", kThin);
        const auto found = detect(root, root_lookup());
        check(found.scan.outcome == NoctaliaScan::kLogin && found.scan.service == "login",
              "a lone non-Noctalia match no longer qualifies");
    }
    {
        const auto root = base / "vendor";
        write(root / kHelperRelative, kHelperBytes, 0755);
        write(root / "usr/lib/pam.d/noctalia", kThin);
        const auto found = detect(root, root_lookup());
        check(found.scan.outcome == NoctaliaScan::kService && found.scan.service == "noctalia",
              "a vendor file in /usr/lib/pam.d is detected");
    }
    {
        const auto root = base / "override";
        write(root / kHelperRelative, kHelperBytes, 0755);
        write(root / "usr/lib/pam.d/noctalia", kThin);
        write(root / "etc/pam.d/noctalia", "auth required pam_unix.so\n");
        check(detect(root, root_lookup()).scan.outcome == NoctaliaScan::kLogin,
              "/etc/pam.d overrides the vendor copy when judging it");
    }
    {
        const auto root = base / "child";
        write(root / kHelperRelative, std::string("smile2unlock-noctalia-auth\0", 27), 0755);
        write(root / "etc/pam.d/smile2unlock-noctalia-auth", kThin);
        check(detect(root, root_lookup()).scan.outcome == NoctaliaScan::kLogin,
              "our own substack is never selected");
    }
}

PlanContext context_for(uid_t uid) {
    return {.helper = root_lookup(), .requester_uid = uid};
}

void vendor_round_trip(const std::filesystem::path& base) {
    const auto root = base / "vendor-flow";
    write(root / kHelperRelative, kHelperBytes, 0755);
    write(root / "usr/lib/pam.d/noctalia", kThin);
    write(root / "etc/pam.d/login", "auth required pam_unix.so\n");
    write(root / "etc/passwd", "me:x:" + std::to_string(::geteuid()) + ":1::/:/bin/sh\n");
    write(root / "etc/shadow", "me:$6$x:1::::::\n", 0600);
    const auto plan = plan_pam_integration(root, TargetKind::kNoctalia, false,
                                           context_for(::geteuid()));
    check(plan.has_value() && plan->source_path == root / "usr/lib/pam.d/noctalia"
              && plan->destination_path == root / "etc/pam.d/noctalia",
          "a vendor file is read from /usr/lib and written to /etc like other targets");
    if (!plan) {
        return;
    }
    check(apply_pam_plan(root, *plan).has_value(), "the vendor plan applies");
    check(read(root / "usr/lib/pam.d/noctalia") == kThin, "the vendor file is untouched");
    check(rollback_pam_integration(root, TargetKind::kNoctalia).has_value()
              && !std::filesystem::exists(root / "etc/pam.d/noctalia"),
          "rollback removes the override and re-exposes the vendor file");
}

// --- 1. rollback validation -------------------------------------------------

void rollback_validates_service(const std::filesystem::path& base) {
    constexpr std::array bad_services{"login", "sudo", "smile2unlock-noctalia-auth",
                                      "other-service", "../noctalia", ""};
    for (const auto [index, bad] : std::views::enumerate(bad_services)) {
        const auto root = base / ("rollback-" + std::to_string(index));
        write(root / kHelperRelative, kHelperBytes, 0755);
        write(root / "etc/pam.d/noctalia", kThin);
        write(root / "etc/passwd", "me:x:" + std::to_string(::geteuid()) + ":1::/:/bin/sh\n");
        write(root / "etc/shadow", "me:$6$x:1::::::\n", 0600);
        const auto plan = plan_pam_integration(root, TargetKind::kNoctalia, false,
                                               context_for(::geteuid()));
        if (!plan || !apply_pam_plan(root, *plan)) {
            check(false, "a rollback fixture applies");
            continue;
        }
        const auto journal_file = root / "var/lib/smile2unlock/deployment.json";
        auto journal = nlohmann::json::parse(read(journal_file));
        journal["targets"]["noctalia"]["service"] = bad;
        write(journal_file, journal.dump(), 0600);
        const auto result = rollback_pam_integration(root, TargetKind::kNoctalia);
        check(!result && result.error().contains("service"),
              std::string("rollback rejects journal service '") + bad + "'");
        check(read(root / "etc/pam.d/noctalia").contains("smile2unlock-noctalia-auth"),
              "a rejected rollback leaves the managed files alone");
    }
}

// --- 4. empty password ------------------------------------------------------

void empty_password_refusal(const std::filesystem::path& base) {
    const auto root = base / "empty-password";
    write(root / kHelperRelative, kHelperBytes, 0755);
    write(root / "etc/pam.d/noctalia", kThin);
    const auto plan_for = [&](uid_t uid) {
        return plan_pam_integration(root, TargetKind::kNoctalia, false, context_for(uid));
    };
    write(root / "etc/passwd",
          "set:x:1001:1::/:/bin/sh\nempty:x:1002:1::/:/bin/sh\nlocked:x:1003:1::/:/bin/sh\n"
          "inline:abc:1004:1::/:/bin/sh\nblank:x:1005:1::/:/bin/sh\nbare::1006:1::/:/bin/sh\n");
    write(root / "etc/shadow",
          "set:$6$h:1::::::\nempty::1::::::\nlocked:!:1::::::\ninline:*:1::::::\n", 0600);

    const auto empty = plan_for(1002);
    check(!empty && empty.error().contains("empty password")
              && empty.error().contains("unlock"),
          "an empty shadow hash is refused with a clear reason");
    check(plan_for(1006).error_or("").contains("empty password"),
          "an empty hash in passwd is refused");
    check(plan_for(1001).has_value(), "an account with a password is allowed");
    check(plan_for(1003).has_value(), "a locked account is not an empty password");
    check(plan_for(1004).has_value(), "a hash kept in passwd is a set password");
    const auto no_entry = plan_for(1005);
    check(!no_entry && no_entry.error().contains("shadow"),
          "an account without a shadow entry cannot be proven safe");
    check(plan_for(4242).has_value(), "an account outside /etc/passwd is not checked locally");
    const auto anonymous = plan_pam_integration(root, TargetKind::kNoctalia, false,
                                                {.helper = root_lookup(), .requester_uid = {}});
    check(!anonymous && anonymous.error().contains("requesting account"),
          "a plan without a requester is refused");
    std::filesystem::remove(root / "etc/shadow");
    const auto unreadable = plan_for(1001);
    check(!unreadable && unreadable.error().contains("shadow"),
          "an unreadable shadow file fails closed");

    check(account_password_state(root, 4242).value_or(AccountPassword::kSet)
              == AccountPassword::kNotLocal,
          "account_password_state reports non-local accounts");
}

// --- 7. snapshot note -------------------------------------------------------

void snapshot_round_trip() {
    auto snapshot = DeploymentSnapshot{};
    snapshot.targets.push_back(TargetStatus{
        .kind = TargetKind::kNoctalia, .role = TargetRole::kLock, .state = TargetState::kSupported,
        .service = "noctalia", .effective_path = "/etc/pam.d/noctalia", .password_fallback = true,
        .authtok_source = AuthtokSource::kNone, .detail = "recognized", .note = "target.noctalia_press_enter",
        .adoptable = false, .adoption_unavailable_reason = "n/a"});
    snapshot.targets.push_back(TargetStatus{
        .kind = TargetKind::kSudo, .role = TargetRole::kPrivilege, .state = TargetState::kManaged,
        .service = "sudo", .authtok_source = AuthtokSource::kSealed, .sealed_available = true});
    const auto text = snapshot_json(snapshot);
    check(nlohmann::json::parse(text)["targets"][0]["note"] == "target.noctalia_press_enter",
          "snapshot_json carries the note");
    const auto parsed = parse_snapshot_json(text);
    check(parsed.has_value() && parsed->targets.size() == 2, "the snapshot parses back");
    if (!parsed || parsed->targets.size() != 2) {
        return;
    }
    const auto& noctalia = parsed->targets[0];
    check(noctalia.note == "target.noctalia_press_enter" && noctalia.service == "noctalia"
              && noctalia.state == TargetState::kSupported && noctalia.role == TargetRole::kLock
              && noctalia.effective_path == "/etc/pam.d/noctalia" && noctalia.password_fallback
              && noctalia.detail == "recognized" && noctalia.adoption_unavailable_reason == "n/a",
          "the note and the other fields survive the round trip");
    check(parsed->targets[1].authtok_source == AuthtokSource::kSealed
              && parsed->targets[1].sealed_available && parsed->targets[1].note.empty(),
          "an empty note stays empty");
    check(!parse_snapshot_json("not json") && !parse_snapshot_json("{}")
              && !parse_snapshot_json(R"({"targets":[{"id":"bogus"}]})")
              && !parse_snapshot_json(R"({"targets":[1]})"),
          "malformed snapshots are rejected");
}

} // namespace

bool noctalia_hardening_tests(const std::filesystem::path& base) {
    const auto before = failures;
    predicate_name_branches();
    predicate_deny_list();
    predicate_wrapper_shapes();
    root_mode_ignores_environment(base);
    root_mode_trust_checks(base);
    helper_reading_rules(base);
    capped_reader(base);
    detection_cache(base);
    service_selection(base);
    vendor_round_trip(base);
    rollback_validates_service(base);
    empty_password_refusal(base);
    snapshot_round_trip();
    return failures == before;
}
