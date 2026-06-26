#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace su::recognizer {

enum class RecognizerError {
    kNoCamera,
    kCameraUnavailable,
    kModelUnavailable,
};

struct CameraInfo {
    int index = 0;
    std::string name;
};

struct PreviewFrame {
    int width = 0;
    int height = 0;
    std::vector<std::byte> rgba_or_rgb;
};

struct FaceBox {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct RecognitionResult {
    bool has_face = false;
    std::optional<FaceBox> face_box;
    std::vector<float> feature;
    float liveness_score = 0.0F;
};

class RecognizerService {
public:
    std::vector<CameraInfo> enumerate_cameras() const;
    std::expected<void, RecognizerError> open_camera(int camera_index);
    std::expected<PreviewFrame, RecognizerError> capture_preview_frame() const;
    std::expected<RecognitionResult, RecognizerError> extract_features() const;
    std::expected<float, RecognizerError> compare_features(
        std::span<const float> lhs,
        std::span<const float> rhs) const;
    void close_camera();

private:
    std::optional<int> active_camera_;
};

}  // namespace su::recognizer

