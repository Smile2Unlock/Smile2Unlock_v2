#include "../auth_service/recognition_agent_protocol.h"

#include <windows.h>

import std;
import su.recognizer.service;

namespace {

using smile2unlock::recognition_agent_ipc::AgentStatus;
using smile2unlock::recognition_agent_ipc::Request;
using smile2unlock::recognition_agent_ipc::Response;

bool read_exact(HANDLE handle, void* data, DWORD size) {
    auto* bytes = static_cast<std::byte*>(data);
    DWORD total = 0;
    while (total < size) {
        DWORD read = 0;
        if (!ReadFile(handle, bytes + total, size - total, &read, nullptr) || read == 0) {
            return false;
        }
        total += read;
    }
    return true;
}

bool write_exact(HANDLE handle, const void* data, DWORD size) {
    const auto* bytes = static_cast<const std::byte*>(data);
    DWORD total = 0;
    while (total < size) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes + total, size - total, &written, nullptr)
            || written == 0) {
            return false;
        }
        total += written;
    }
    return true;
}

void wipe(Response& response) {
    SecureZeroMemory(response.feature.data(), response.feature.size() * sizeof(float));
}

} // namespace

int wmain() {
    Request request{};
    Response response{};
    response.nonce = request.nonce;
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE
        || input == nullptr || output == nullptr
        || !read_exact(input, &request, sizeof(request))) {
        response.status = AgentStatus::kInvalidRequest;
        (void)write_exact(output, &response, sizeof(response));
        wipe(response);
        return 2;
    }
    response.nonce = request.nonce;
    if (request.magic != smile2unlock::recognition_agent_ipc::kRequestMagic
        || request.version != smile2unlock::recognition_agent_ipc::kVersion
        || request.camera_index < 0
        || request.timeout_ms < 1000
        || request.timeout_ms > 30'000
        || !std::isfinite(request.liveness_threshold)
        || request.liveness_threshold < 0.0F
        || request.liveness_threshold > 1.0F) {
        response.status = AgentStatus::kInvalidRequest;
        (void)write_exact(output, &response, sizeof(response));
        wipe(response);
        return 2;
    }

    su::recognizer::RecognizerService recognizer;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds{request.timeout_ms};
    const auto open_deadline = std::min(
        deadline, std::chrono::steady_clock::now() + std::chrono::seconds{2});
    auto camera_open = false;
    do {
        if (recognizer.open_camera(request.camera_index)) {
            camera_open = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    } while (std::chrono::steady_clock::now() < open_deadline);
    if (!camera_open) {
        response.status = AgentStatus::kCameraUnavailable;
        (void)write_exact(output, &response, sizeof(response));
        wipe(response);
        return 3;
    }
    if (const auto reset = recognizer.reset_liveness(); !reset) {
        response.status = AgentStatus::kModelUnavailable;
        (void)write_exact(output, &response, sizeof(response));
        wipe(response);
        return 4;
    }

    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = recognizer.extract_features(true);
        if (!result || !result->has_face || result->feature.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{80});
            continue;
        }
        if (result->liveness_score < request.liveness_threshold
            || result->feature.size() > response.feature.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{80});
            continue;
        }
        response.status = AgentStatus::kOk;
        response.feature_count = static_cast<std::uint32_t>(result->feature.size());
        response.liveness_score = result->liveness_score;
        std::copy(result->feature.begin(), result->feature.end(), response.feature.begin());
        (void)write_exact(output, &response, sizeof(response));
        wipe(response);
        request = {};
        return 0;
    }
    response.status = AgentStatus::kTimedOut;
    (void)write_exact(output, &response, sizeof(response));
    wipe(response);
    request = {};
    return 5;
}
