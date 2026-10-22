#include "common/log/logger.h"
#include "common/capture/image.h"
#include "../auth_service/recognition_agent_protocol.h"

#include <windows.h>

import std;
import su.recognizer.service;
import su.recognizer.types;

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
    SecureZeroMemory(&response, sizeof(response));
}

void attach_image(const Request& request, Response& response,
                  const su::recognizer::PreviewFrame& frame) {
    if (!(request.flags & smile2unlock::recognition_agent_ipc::kCaptureRequested)) return;
    try {
        const auto jpeg = su::capture::prepare_image(frame.rgba_or_rgb, frame.width, frame.height);
        if (!jpeg) return;
        response.jpeg_size = static_cast<std::uint32_t>(jpeg->bytes.size());
        response.width = jpeg->width;
        response.height = jpeg->height;
        std::ranges::copy(jpeg->bytes, response.jpeg.begin());
    } catch (...) { /* optional evidence never changes recognition */ }
}

} // namespace

int wmain() {
    const auto& logger = su::log::process_logger("recognition-agent", true);
    logger.info("agent.started");
    Request request{};
    Response response{};
    response.nonce = request.nonce;
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE
        || input == nullptr || output == nullptr
        || !read_exact(input, &request, sizeof(request))) {
        logger.warn("request.invalid");
        response.status = AgentStatus::kInvalidRequest;
        (void)write_exact(output, &response, sizeof(response));
        wipe(response);
        return 2;
    }
    response.nonce = request.nonce;
    if (request.magic != smile2unlock::recognition_agent_ipc::kRequestMagic
        || request.version != smile2unlock::recognition_agent_ipc::kVersion
        || (request.flags & ~smile2unlock::recognition_agent_ipc::kCaptureRequested) != 0
        || request.camera_index < 0
        || request.timeout_ms < 1000
        || request.timeout_ms > 30'000
        || !std::isfinite(request.liveness_threshold)
        || request.liveness_threshold < 0.0F
        || request.liveness_threshold > 1.0F) {
        logger.warn("request.invalid");
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
        logger.debug("camera.open_attempt", {su::log::field("camera_index", request.camera_index)});
        if (recognizer.open_camera(request.camera_index)) {
            camera_open = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    } while (std::chrono::steady_clock::now() < open_deadline);
    if (!camera_open) {
        logger.error("camera.open_failed", {su::log::field("camera_index", request.camera_index)});
        response.status = AgentStatus::kCameraUnavailable;
        (void)write_exact(output, &response, sizeof(response));
        wipe(response);
        return 3;
    }
    const auto liveness_enabled = request.liveness_threshold > 0.0F;
    if (liveness_enabled && !recognizer.reset_liveness()) {
        logger.error("model.unavailable");
        response.status = AgentStatus::kModelUnavailable;
        (void)write_exact(output, &response, sizeof(response));
        wipe(response);
        return 4;
    }

    auto saw_face = false;
    auto best_frame = su::recognizer::PreviewFrame{};
    auto best_score = -1.0F;
    while (std::chrono::steady_clock::now() < deadline) {
        auto captured = recognizer.capture_and_extract(liveness_enabled);
        auto result = captured
            ? std::expected<su::recognizer::RecognitionResult, su::recognizer::RecognizerError>{std::move(captured->second)}
            : std::unexpected(captured.error());
        if (!result && result.error() == su::recognizer::RecognizerError::kModelUnavailable) {
            logger.error("model.unavailable");
            response.status = AgentStatus::kModelUnavailable;
            (void)write_exact(output, &response, sizeof(response));
            wipe(response);
            return 4;
        }
        if (!result || !result->has_face) {
            std::this_thread::sleep_for(std::chrono::milliseconds{80});
            continue;
        }
        saw_face = true;
        if ((request.flags & smile2unlock::recognition_agent_ipc::kCaptureRequested)
            && result->liveness_score >= best_score) {
            best_frame = std::move(captured->first);
            best_score = result->liveness_score;
        }
        if (result->feature.empty() || result->liveness_score < request.liveness_threshold
            || result->feature.size() > response.feature.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{80});
            continue;
        }
        logger.info("recognition.accepted");
        response.status = AgentStatus::kOk;
        response.feature_count = static_cast<std::uint32_t>(result->feature.size());
        response.liveness_score = result->liveness_score;
        std::copy(result->feature.begin(), result->feature.end(), response.feature.begin());
        attach_image(request, response, best_frame);
        (void)write_exact(output, &response, sizeof(response));
        wipe(response);
        request = {};
        return 0;
    }
    logger.warn("recognition.timed_out");
    response.status = saw_face ? AgentStatus::kNoLiveFace : AgentStatus::kTimedOut;
    if (saw_face) attach_image(request, response, best_frame);
    (void)write_exact(output, &response, sizeof(response));
    wipe(response);
    request = {};
    return 5;
}
