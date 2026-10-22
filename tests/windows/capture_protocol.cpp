#include "recognition_result.h"
#include <cstdlib>
#include <iostream>

void check(bool value) { if (!value) std::abort(); }
int main() {
    using namespace smile2unlock::recognition_agent_ipc;
    using su::windows::auth_service::validate_agent_result;
    auto request = Request{};
    auto response = Response{};
    response.status = AgentStatus::kOk;
    response.feature_count = 1;
    response.feature[0] = 0.5F;
    response.liveness_score = 1;
    check(validate_agent_result(request, response, 0) == ERROR_SUCCESS);
    response.jpeg_size = 4;
    response.width = 320;
    response.height = 240;
    check(validate_agent_result(request, response, 0) == ERROR_INVALID_DATA);
    request.flags = kCaptureRequested;
    check(validate_agent_result(request, response, 0) == ERROR_SUCCESS);
    response.jpeg_size = kMaximumJpegBytes + 1;
    check(validate_agent_result(request, response, 0) == ERROR_INVALID_DATA);
    response.jpeg_size = kMaximumJpegBytes;
    response.width = 321;
    check(validate_agent_result(request, response, 0) == ERROR_INVALID_DATA);
    response.width = 320;
    response.nonce[0] = 1;
    check(validate_agent_result(request, response, 0) == ERROR_INVALID_DATA);
    response.nonce = request.nonce;
    response.status = AgentStatus::kTimedOut;
    check(validate_agent_result(request, response, 5) == ERROR_INVALID_DATA);
    response.status = AgentStatus::kNoLiveFace;
    check(validate_agent_result(request, response, 5) == ERROR_LOGON_FAILURE);
    request.flags = 2;
    check(validate_agent_result(request, response, 5) == ERROR_INVALID_DATA);
    using Settings = smile2unlock::logon_secret_ipc::RecognitionSettingsPayload;
    static_assert(sizeof(Settings) == 44);
    constexpr Settings settings{};
    static_assert(settings.capture_enabled == 0 && settings.capture_retention_days == 7 && settings.capture_max_count == 50);
    static_assert(kMaximumJpegBytes < smile2unlock::logon_secret_ipc::kPayloadCapacity);
    std::cout << "capture opt-in, bounds, nonce, no-face and settings protocol passed\n";
}
