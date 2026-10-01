#include "recognition_result.h"

#include <iostream>
#include <limits>

using namespace smile2unlock;
using su::windows::auth_service::recognition_error_status;
using su::windows::auth_service::validate_agent_result;

int main() {
    using recognition_agent_ipc::AgentStatus;
    using logon_secret_ipc::Status;
    const auto request = recognition_agent_ipc::Request{};
    auto response = recognition_agent_ipc::Response{};
    // Agent main emits nonzero exits for these outcomes. Test that the wire
    // status survives this path, instead of all errors becoming mismatch.
    struct Case { AgentStatus agent; DWORD exit; Status broker; };
    const Case cases[]{
        {AgentStatus::kModelUnavailable, 4, Status::kUnavailable},
        {AgentStatus::kInternalError, 4, Status::kUnavailable},
        {AgentStatus::kInvalidRequest, 2, Status::kInvalidRequest},
        {AgentStatus::kCameraUnavailable, 3, Status::kAuthenticationFailed},
        {AgentStatus::kTimedOut, 5, Status::kAuthenticationFailed},
        {AgentStatus::kNoLiveFace, 5, Status::kAuthenticationFailed},
    };
    for (const auto& value : cases) {
        response.status = value.agent;
        if (recognition_error_status(validate_agent_result(request, response, value.exit)) != value.broker) {
            std::cerr << "agent failure lost its retry classification\n";
            return 1;
        }
    }
    for (const auto error : {ERROR_FILE_NOT_FOUND, ERROR_MOD_NOT_FOUND, ERROR_BAD_EXE_FORMAT}) {
        if (recognition_error_status(error) != Status::kUnavailable) { return 2; }
    }
    if (recognition_error_status(ERROR_ACCESS_DENIED) != Status::kAccessDenied) { return 3; }
    response.status = AgentStatus::kTimedOut;
    response.nonce[0] = 1;
    if (recognition_error_status(validate_agent_result(request, response, 5)) != Status::kCorrupt) {
        std::cerr << "bad evidence nonce accepted as retryable timeout\n";
        return 4;
    }
    response.nonce = request.nonce;
    response.status = AgentStatus::kOk;
    response.feature_count = 1;
    response.feature[0] = 0.25F;
    response.liveness_score = request.liveness_threshold;
    if (validate_agent_result(request, response, 0) != ERROR_SUCCESS) { return 5; }
    if (recognition_error_status(validate_agent_result(request, response, 4)) != Status::kUnavailable) { return 6; }
    response.feature[0] = std::numeric_limits<float>::quiet_NaN();
    if (recognition_error_status(validate_agent_result(request, response, 0)) != Status::kCorrupt) { return 7; }
    response.feature[0] = 0.25F;
    response.feature_count = static_cast<std::uint32_t>(response.feature.size() + 1);
    if (validate_agent_result(request, response, 0) != ERROR_INVALID_DATA) { return 8; }
    std::cout << "recognition result envelope and broker classification passed\n";
}
