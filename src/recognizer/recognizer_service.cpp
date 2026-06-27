#include "recognizer/recognizer_service.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>
#include <ranges>
#include <sstream>

namespace su::recognizer {

std::vector<CameraInfo> RecognizerService::enumerate_cameras() const {
    return {
        CameraInfo{.index = 0, .name = "Mock camera 0"},
    };
}

std::expected<void, RecognizerError> RecognizerService::open_camera(int camera_index) {
    const auto cameras = enumerate_cameras();
    const auto exists = std::ranges::any_of(
        cameras,
        [camera_index](const CameraInfo& camera) {
            return camera.index == camera_index;
        });
    if (!exists) {
        return std::unexpected(RecognizerError::kNoCamera);
    }

    active_camera_ = camera_index;
    return {};
}

std::expected<PreviewFrame, RecognizerError> RecognizerService::capture_preview_frame() const {
    if (!active_camera_) {
        return std::unexpected(RecognizerError::kCameraUnavailable);
    }

    constexpr int width = 2;
    constexpr int height = 2;
    return PreviewFrame{
        .width = width,
        .height = height,
        .rgba_or_rgb = std::vector<std::byte>(width * height * 4, std::byte{0x80}),
    };
}

std::expected<RecognitionResult, RecognizerError> RecognizerService::extract_features() const {
    if (!active_camera_) {
        return std::unexpected(RecognizerError::kCameraUnavailable);
    }

    return RecognitionResult{
        .has_face = true,
        .face_box = FaceBox{.x = 10, .y = 12, .width = 96, .height = 96},
        .feature = {0.10F, 0.20F, 0.30F, 0.40F},
        .liveness_score = 0.95F,
    };
}

std::expected<float, RecognizerError> RecognizerService::compare_features(
    std::span<const float> lhs,
    std::span<const float> rhs) const {
    if (lhs.size() != rhs.size() || lhs.empty()) {
        return std::unexpected(RecognizerError::kModelUnavailable);
    }

    const auto squared_distance = std::transform_reduce(
        lhs.begin(),
        lhs.end(),
        rhs.begin(),
        0.0F,
        std::plus<>{},
        [](float left, float right) {
            const auto delta = left - right;
            return delta * delta;
        });
    return 1.0F / (1.0F + std::sqrt(squared_distance));
}

void RecognizerService::close_camera() {
    active_camera_.reset();
}

std::string embedding_sample_source(std::span<const float> feature) {
    auto out = std::ostringstream{};
    out << "embedding:";
    for (std::size_t index = 0; index < feature.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << std::format("{:.9g}", feature[index]);
    }
    return out.str();
}

}  // namespace su::recognizer
