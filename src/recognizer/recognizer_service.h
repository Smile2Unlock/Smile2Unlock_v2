#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace su::recognizer {

class SeetaFaceBackend;
class V4L2Camera;

enum class RecognizerError {
    kNoCamera,
    kCameraUnavailable,
    kModelUnavailable,
    kInvalidArgument,
    kInvalidImage,
    kImageLoadFailed,
    kNoFace,
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

// Non-owning view over decoded image pixels. Channels must be 1, 3, or 4 to be
// accepted by the SeetaFace backend.
struct ImageView {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::span<const std::byte> bytes;
};

// A captured camera frame in its native V4L2 pixel format. The bytes are
// non-owning (they point at the mmap'ed V4L2 buffer) and only valid until the
// next grab_frame call. The v4l2_format field carries a V4L2_PIX_FMT_* value
// interpreted by v4l2_frame_to_rgb in pixel_convert.cpp.
struct CapturedFrame {
    int width = 0;
    int height = 0;
    std::uint32_t v4l2_format = 0;
    std::span<const std::byte> bytes;
};

class RecognizerService {
public:
    RecognizerService();
    ~RecognizerService();

    RecognizerService(const RecognizerService&) = delete;
    RecognizerService& operator=(const RecognizerService&) = delete;
    RecognizerService(RecognizerService&&) noexcept;
    RecognizerService& operator=(RecognizerService&&) noexcept;

    std::vector<CameraInfo> enumerate_cameras() const;
    std::expected<void, RecognizerError> open_camera(int camera_index);
    std::expected<PreviewFrame, RecognizerError> capture_preview_frame() const;
    std::expected<RecognitionResult, RecognizerError> extract_features() const;
std::expected<float, RecognizerError> compare_features(
        std::span<const float> lhs,
        std::span<const float> rhs) const;
    void close_camera();

    // Capture one frame and run detection on that same frame, returning both.
    // This avoids the double-grab (and face-box/preview frame mismatch) that
    // separate capture_preview_frame + extract_features calls would cause,
    // since each grab_frame returns a span valid only until the next grab.
    std::expected<std::pair<PreviewFrame, RecognitionResult>, RecognizerError>
    capture_and_extract() const;

    // Extract features from a decoded image (e.g. a static face photo).
    // Requires the SeetaFace backend; when the backend is unavailable this
    // returns kModelUnavailable so callers can fall back to mock sources.
    std::expected<RecognitionResult, RecognizerError> extract_from_image(
        ImageView image, bool liveness_enabled = true) const;

    // Detect the face and run anti-spoofing Predict only (no feature
    // extraction). Intended to be called on every preview frame. When
    // liveness_enabled is false, Predict is skipped and liveness_score=1.0 so
    // the frame still has a face box without the liveness gate; callers gate
    // this on config.liveness_detection.
    std::expected<RecognitionResult, RecognizerError> predict_liveness(
        ImageView image, bool liveness_enabled = true) const;

    // Whether the real SeetaFace recognizer backend is available. False when
    // SeetaFace is compiled out (SU_HAS_SEETAFACE=0) or model assets are
    // missing at the resolved model directory.
    bool seetaface_available() const;

private:
    std::expected<void, RecognizerError> ensure_seetaface_backend() const;

    std::optional<int> active_camera_;
#if SU_HAS_SEETAFACE
    // Lazily loaded on first extract_from_image / seetaface_available call so
    // that app startup does not pay the model-load cost until needed. Protected
    // by a mutable mutex (double-checked locking) because ensure_seetaface_backend
    // is const and may be called concurrently from the preview and detect threads.
    // Stored as a unique_ptr (with a forward-declared type) so this header does
    // not need to include seetaface_backend.h, avoiding a circular include.
    mutable std::unique_ptr<SeetaFaceBackend> seetaface_backend_;
    mutable std::unique_ptr<std::mutex> backend_mutex_;
#endif
    // V4L2 capture device. Owned via unique_ptr with a forward-declared type so
    // this header stays free of platform (videodev2.h) includes.
    std::unique_ptr<V4L2Camera> camera_;
};

std::string embedding_sample_source(std::span<const float> feature);

}  // namespace su::recognizer
