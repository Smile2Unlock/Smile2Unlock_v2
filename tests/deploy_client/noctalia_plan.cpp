#include "platform/linux/deploy_client/noctalia_plan.h"

#include <iostream>
#include <string>

namespace {

using namespace su::deploy;

int failures = 0;

void check(bool ok, std::string_view message) {
    if (!ok) {
        std::cerr << "FAIL noctalia plan: " << message << '\n';
        ++failures;
    }
}

std::string plan_text(std::string_view service = "noctalia",
                      std::string_view destination = "/etc/pam.d/noctalia",
                      std::string_view source = "/etc/pam.d/noctalia",
                      std::string_view target = "noctalia",
                      std::string_view substack = "/etc/pam.d/smile2unlock-noctalia-auth") {
    return R"({"version":1,"target":")" + std::string(target) + R"(","service":")"
        + std::string(service) + R"(","source":")" + std::string(source) + R"(","destination":")"
        + std::string(destination) + R"(","substack":")" + std::string(substack)
        + R"(","authtok_source":"none","already_managed":false,"password_fallback":true})";
}

NoctaliaExpectation expected() {
    return {.service = "noctalia", .source = "/etc/pam.d/noctalia"};
}

bool rejected(const std::string& plan, const NoctaliaExpectation& want, std::string_view fragment) {
    const auto result = verify_noctalia_plan(plan, want, noctalia_service_policy());
    return !result && result.error().contains(fragment);
}

void plan_verification() {
    const auto policy = noctalia_service_policy();
    check(verify_noctalia_plan(plan_text(), expected(), policy).has_value(),
          "a plan matching the inspection is accepted");
    check(verify_noctalia_plan(
              plan_text("noctalia", "/etc/pam.d/noctalia", "/usr/lib/pam.d/noctalia"),
              {.service = "noctalia", .source = "/usr/lib/pam.d/noctalia"}, policy)
              .has_value(),
          "a vendor source is accepted when the GUI saw the same vendor file");
    check(rejected(plan_text("noctalia-other", "/etc/pam.d/noctalia-other"), expected(),
                   "noctalia-other"),
          "a different service than inspected is refused and named");
    check(rejected(plan_text("noctalia", "/etc/pam.d/sudo"), expected(), "/etc/pam.d/sudo"),
          "a different destination is refused and names the file");
    check(rejected(plan_text("sudo", "/etc/pam.d/sudo", "/etc/pam.d/sudo"),
                   {.service = "sudo", .source = "/etc/pam.d/sudo"}, "another component"),
          "the predicate refuses a privileged service even if both sides agree");
    check(rejected(plan_text("login", "/etc/pam.d/login", "/etc/pam.d/login"),
                   {.service = "login", .source = "/etc/pam.d/login"}, "another component"),
          "the login service is refused");
    check(rejected(plan_text("noctalia", "/etc/pam.d/noctalia", "/etc/pam.d/noctalia", "sudo"),
                   expected(), "target"),
          "a plan for another target is refused");
    check(rejected(plan_text("noctalia", "/etc/pam.d/noctalia", "/etc/pam.d/elsewhere"),
                   expected(), "source"),
          "a different source file is refused");
    check(rejected(plan_text("noctalia", "/etc/pam.d/noctalia", "/etc/pam.d/noctalia", "noctalia",
                             "/etc/pam.d/login"),
                   expected(), "substack"),
          "a different substack is refused");
    check(rejected("not json", expected(), "unreadable"), "a malformed plan is refused");
    check(rejected("{}", expected(), "service ''"), "a plan without a service is refused");
    check(rejected(plan_text("../etc/passwd"), expected(), "plain file name"),
          "a path-like service is refused");
}

DeploymentSnapshot snapshot_with(TargetState state, std::string_view service) {
    auto snapshot = DeploymentSnapshot{};
    snapshot.targets.push_back(TargetStatus{
        .kind = TargetKind::kNoctalia, .role = TargetRole::kLock, .state = state,
        .service = std::string(service), .effective_path = "/etc/pam.d/noctalia",
        .detail = "why not"});
    return snapshot;
}

void expectation_rules() {
    const auto policy = noctalia_service_policy();
    const auto good = noctalia_expectation(snapshot_with(TargetState::kSupported, "noctalia"), policy);
    check(good && good->service == "noctalia" && good->source == "/etc/pam.d/noctalia",
          "a supported target yields the expectation");
    check(noctalia_expectation(snapshot_with(TargetState::kManaged, "noctalia"), policy).has_value(),
          "a managed target is re-applicable");
    const auto unsupported = noctalia_expectation(snapshot_with(TargetState::kUnsupported, ""), policy);
    check(!unsupported && unsupported.error().contains("why not"),
          "an unsupported target is refused with its detail");
    check(!noctalia_expectation(snapshot_with(TargetState::kSupported, "sudo"), policy),
          "an unacceptable inspected service is refused");
    check(!noctalia_expectation({}, policy), "a snapshot without the target is refused");
}

} // namespace

bool noctalia_plan_tests() {
    const auto before = failures;
    plan_verification();
    expectation_rules();
    return failures == before;
}
