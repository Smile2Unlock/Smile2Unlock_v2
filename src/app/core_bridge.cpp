#include "app/core_bridge.h"

#include <string>

extern "C" {

enum SuStatus {
    SuStatus_Ok = 0,
    SuStatus_NullArgument = 1,
    SuStatus_InvalidUtf8 = 2,
    SuStatus_UserDenied = 3,
};

struct SuAuthDecision {
    SuStatus status;
    bool accepted;
};

std::uint32_t su_core_version_major();
SuAuthDecision su_core_evaluate_auth(
    const char* username,
    float similarity,
    float threshold,
    bool liveness_ok);
SuStatus su_core_default_threshold(float* out_threshold);

}  // extern "C"

namespace su::app {

namespace {

CoreError map_status(SuStatus status) {
    switch (status) {
    case SuStatus_Ok:
        return CoreError::kUnknown;
    case SuStatus_NullArgument:
        return CoreError::kNullArgument;
    case SuStatus_InvalidUtf8:
        return CoreError::kInvalidUtf8;
    case SuStatus_UserDenied:
        return CoreError::kUserDenied;
    }
    return CoreError::kUnknown;
}

}  // namespace

std::uint32_t core_version_major() {
    return su_core_version_major();
}

std::expected<float, CoreError> default_threshold() {
    float threshold = 0.0F;
    const auto status = su_core_default_threshold(&threshold);
    if (status != SuStatus_Ok) {
        return std::unexpected(map_status(status));
    }
    return threshold;
}

std::expected<AuthDecision, CoreError> evaluate_auth(
    std::string_view username,
    float similarity,
    float threshold,
    bool liveness_ok) {
    const auto owned_username = std::string(username);
    const auto decision = su_core_evaluate_auth(
        owned_username.c_str(),
        similarity,
        threshold,
        liveness_ok);
    if (decision.status != SuStatus_Ok) {
        return std::unexpected(map_status(decision.status));
    }
    return AuthDecision{.accepted = decision.accepted};
}

}  // namespace su::app

