#pragma once

#include "logon_secret_protocol.h"
#include "recognition_agent_protocol.h"

#include <algorithm>
#include <cmath>

namespace su::windows::auth_service {

// Validate the evidence envelope before trusting a status. Nonzero agent exit
// codes accompany legitimate camera/model failures and must not erase them.
inline DWORD validate_agent_result(
    const smile2unlock::recognition_agent_ipc::Request& request,
    const smile2unlock::recognition_agent_ipc::Response& response,
    DWORD exit_code) {
    using namespace smile2unlock::recognition_agent_ipc;
    if (response.magic != kResponseMagic || response.version != kVersion
        || response.nonce != request.nonce) {
        return ERROR_INVALID_DATA;
    }
    switch (response.status) {
    case AgentStatus::kInvalidRequest: return ERROR_INVALID_PARAMETER;
    case AgentStatus::kCameraUnavailable: return ERROR_BUSY;
    case AgentStatus::kModelUnavailable: return ERROR_MOD_NOT_FOUND;
    case AgentStatus::kNoLiveFace: return ERROR_LOGON_FAILURE;
    case AgentStatus::kTimedOut: return ERROR_TIMEOUT;
    case AgentStatus::kInternalError: return ERROR_GEN_FAILURE;
    case AgentStatus::kOk: break;
    default: return ERROR_INVALID_DATA;
    }
    if (exit_code != ERROR_SUCCESS) { return ERROR_GEN_FAILURE; }
    if (response.feature_count == 0 || response.feature_count > response.feature.size()
        || !std::isfinite(response.liveness_score)
        || response.liveness_score < request.liveness_threshold
        || !std::all_of(response.feature.begin(),
            response.feature.begin() + response.feature_count,
            [](float value) { return std::isfinite(value); })) {
        return ERROR_INVALID_DATA;
    }
    return ERROR_SUCCESS;
}

// Keep hard failures distinct all the way to the CP's retry classifier. This
// uses existing wire statuses, so older clients still stop safely.
inline smile2unlock::logon_secret_ipc::Status recognition_error_status(DWORD error) {
    using smile2unlock::logon_secret_ipc::Status;
    switch (error) {
    case ERROR_BUSY:
    case ERROR_NOT_READY:
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_LOGON_FAILURE:
    case ERROR_TIMEOUT:
    case ERROR_CANCELLED:
    case ERROR_OPERATION_ABORTED:
        return Status::kAuthenticationFailed;
    case ERROR_ACCESS_DENIED: return Status::kAccessDenied;
    case ERROR_INVALID_PARAMETER: return Status::kInvalidRequest;
    case ERROR_INVALID_DATA: return Status::kCorrupt;
    default: return Status::kUnavailable;
    }
}

} // namespace su::windows::auth_service
