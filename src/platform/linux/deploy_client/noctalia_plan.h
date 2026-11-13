#pragma once

#include "platform/linux/deploy/deployment.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace su::deploy {

// What the unprivileged GUI itself saw when it inspected Noctalia. The root
// helper plans independently, so the client compares the two before Apply.
struct NoctaliaExpectation {
    std::string service;
    std::filesystem::path source;
};

// Fails (with a user-facing reason) unless Noctalia is configurable and its
// service satisfies the predicate.
[[nodiscard]] std::expected<NoctaliaExpectation, std::string> noctalia_expectation(
    const DeploymentSnapshot& snapshot, const ServicePolicy& policy);

// Aborts on any disagreement between the plan reply and what the GUI inspected,
// or with the predicate. The message names the file the helper wants to modify.
[[nodiscard]] std::expected<void, std::string> verify_noctalia_plan(
    std::string_view plan_json, const NoctaliaExpectation& expected, const ServicePolicy& policy);

} // namespace su::deploy
