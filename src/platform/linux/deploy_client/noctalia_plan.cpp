#include "platform/linux/deploy_client/noctalia_plan.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>

namespace su::deploy {
namespace {

constexpr std::string_view kPamDirectory = "/etc/pam.d/";

std::string text_field(const nlohmann::json& plan, const char* key) {
    const auto found = plan.find(key);
    return found != plan.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::unexpected<std::string> mismatch(std::string_view what, std::string_view planned,
                                      std::string_view inspected) {
    return std::unexpected(std::format(
        "refusing to apply: the deployment helper planned {} '{}' but this app inspected '{}'",
        what, planned, inspected));
}

} // namespace

std::expected<NoctaliaExpectation, std::string> noctalia_expectation(
    const DeploymentSnapshot& snapshot, const ServicePolicy& policy) {
    const auto found = std::ranges::find(snapshot.targets, TargetKind::kNoctalia, &TargetStatus::kind);
    if (found == snapshot.targets.end() || (found->state != TargetState::kSupported
                                           && found->state != TargetState::kManaged)) {
        return std::unexpected("Noctalia lock-screen authentication cannot be configured here"
                               + (found == snapshot.targets.end() || found->detail.empty()
                                      ? std::string{} : ": " + found->detail));
    }
    if (const auto accepted = check_noctalia_service_name(found->service, policy); !accepted) {
        return std::unexpected(std::string(service_rejection_text(accepted.error())));
    }
    return NoctaliaExpectation{.service = found->service, .source = found->effective_path};
}

std::expected<void, std::string> verify_noctalia_plan(
    std::string_view plan_json, const NoctaliaExpectation& expected, const ServicePolicy& policy) {
    const auto plan = nlohmann::json::parse(plan_json, nullptr, false);
    if (!plan.is_object()) {
        return std::unexpected("the deployment helper returned an unreadable plan");
    }
    const auto service = text_field(plan, "service");
    const auto destination = text_field(plan, "destination");
    if (const auto accepted = check_noctalia_service_name(service, policy); !accepted) {
        return std::unexpected(std::format(
            "refusing to apply: the helper chose service '{}': {}", service,
            service_rejection_text(accepted.error())));
    }
    if (text_field(plan, "target") != "noctalia") {
        return mismatch("target", text_field(plan, "target"), "noctalia");
    }
    if (service != expected.service) {
        return mismatch("service", service, expected.service);
    }
    // The managed flow always writes under /etc/pam.d, even for a vendor copy.
    const auto wanted_destination = std::string(kPamDirectory) + expected.service;
    if (destination != wanted_destination) {
        return mismatch("to modify", destination, wanted_destination);
    }
    if (text_field(plan, "source") != expected.source.string()) {
        return mismatch("source", text_field(plan, "source"), expected.source.string());
    }
    if (text_field(plan, "substack") != std::string(kPamDirectory) + policy.child_service) {
        return mismatch("substack", text_field(plan, "substack"),
                        std::string(kPamDirectory) + policy.child_service);
    }
    return {};
}

} // namespace su::deploy
