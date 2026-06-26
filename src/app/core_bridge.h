#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace su::app {

enum class CoreError {
    kNullArgument,
    kInvalidUtf8,
    kUserDenied,
    kUnknown,
};

struct AuthDecision {
    bool accepted = false;
};

std::uint32_t core_version_major();
std::expected<float, CoreError> default_threshold();
std::expected<AuthDecision, CoreError> evaluate_auth(
    std::string_view username,
    float similarity,
    float threshold,
    bool liveness_ok);

}  // namespace su::app

