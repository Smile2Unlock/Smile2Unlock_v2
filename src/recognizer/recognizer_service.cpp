#include "recognizer/recognizer_service.h"

#include "recognizer/camera/v4l2_camera.h"
#include "recognizer/image/pixel_convert.h"
#include "recognizer/seetaface_backend.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <numeric>
#include <ranges>
#include <sstream>
#include <utility>

namespace su::recognizer {

RecognizerService::RecognizerService()
    : backend_mutex_(std::make_unique<std::mutex>()) {}

RecognizerService::~RecognizerService() = default;

RecognizerService::RecognizerService(RecognizerService&&) noexcept = default;

RecognizerService& RecognizerService::operator=(RecognizerService&&) noexcept = default;

std::vector<CameraInfo> RecognizerService::enumerate_cameras() const {
    const auto devices = enumerate_v4l2_cameras();
    auto cameras = std::vector<CameraInfo>{};
    cameras.reserve(devices.size());
    for (const auto& device : devices) {
        cameras.push_back(CameraInfo{.index = device.index, .name = device.name});
    }
    // Always expose a mock slot so headless/test environments without a camera
    // can still drive the demo path.
    if (cameras.empty()) {
        cameras.push_back(CameraInfo{.index = 0, .name = "Mock camera 0"});
    }
    return cameras;
}

std::expected<void, RecognizerError> RecognizerService::open_camera(int camera_index) {
    close_camera();

    camera_ = std::make_unique<V4L2Camera>();
    auto opened = camera_->open(camera_index);
    if (opened) {
        active_camera_ = camera_index;
        return {};
    }

    // No physical device or format negotiation failed: fall back to mock mode
    // so the app stays usable in headless/test environments. The caller still
    // sees the camera as "open" and capture_preview_frame returns a mock frame.
    camera_.reset();
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

namespace {

PreviewFrame mock_preview_frame() {
    constexpr int width = 2;
    constexpr int height = 2;
    return PreviewFrame{
        .width = width,
        .height = height,
        .rgba_or_rgb = std::vector<std::byte>(width * height * 3, std::byte{0x80}),
    };
}

}  // namespace

std::expected<PreviewFrame, RecognizerError> RecognizerService::capture_preview_frame() const {
    if (!active_camera_) {
        return std::unexpected(RecognizerError::kCameraUnavailable);
    }
    if (!camera_) {
        return mock_preview_frame();
    }

    auto frame = camera_->grab_frame();
    if (!frame) {
        return std::unexpected(frame.error());
    }
    auto rgb = v4l2_frame_to_rgb(*frame);
    if (!rgb) {
        return std::unexpected(rgb.error());
    }
    return PreviewFrame{
        .width = frame->width,
        .height = frame->height,
        .rgba_or_rgb = std::move(*rgb),
    };
}

namespace {

// Synthetic detection result for the mock 2x2 frame, which has no real face.
// Keeps the headless demo path usable without a camera or SeetaFace.
RecognitionResult mock_recognition_result() {
    return RecognitionResult{
        .has_face = true,
        .face_box = FaceBox{.x = 10, .y = 12, .width = 96, .height = 96},
        .feature = {0.10F, 0.20F, 0.30F, 0.40F},
        .liveness_score = 0.95F,
    };
}

}  // namespace

std::expected<std::pair<PreviewFrame, RecognitionResult>, RecognizerError>
RecognizerService::capture_and_extract() const {
    if (!active_camera_) {
        return std::unexpected(RecognizerError::kCameraUnavailable);
    }

    if (!camera_) {
        return std::make_pair(mock_preview_frame(), mock_recognition_result());
    }

    // Grab exactly once and run detection on the same converted frame, so the
    // face box and the preview image always correspond to the same capture.
    auto frame = camera_->grab_frame();
    if (!frame) {
        return std::unexpected(frame.error());
    }
    auto rgb = v4l2_frame_to_rgb(*frame);
    if (!rgb) {
        return std::unexpected(rgb.error());
    }
    auto preview = PreviewFrame{
        .width = frame->width,
        .height = frame->height,
        .rgba_or_rgb = *rgb,
    };
    auto result = extract_from_image(ImageView{
        .width = frame->width,
        .height = frame->height,
        .channels = 3,
        .bytes = std::span<const std::byte>(preview.rgba_or_rgb),
    });
    if (!result) {
        return std::unexpected(result.error());
    }
    return std::make_pair(std::move(preview), std::move(*result));
}

std::expected<RecognitionResult, RecognizerError> RecognizerService::extract_features() const {
    auto captured = capture_and_extract();
    if (!captured) {
        return std::unexpected(captured.error());
    }
    return std::move(captured->second);
}

namespace {

// Pure function: sum of squared elements for a span of floats. Used by
// cosine similarity to compute norms. Zero-overhead: inlined at call site.
float squared_norm(std::span<const float> values) {
    return std::transform_reduce(
        values.begin(), values.end(), 0.0F, std::plus<>{},
        [](float v) { return v * v; });
}

}  // namespace

std::expected<float, RecognizerError> RecognizerService::compare_features(
    std::span<const float> lhs,
    std::span<const float> rhs) const {
    if (lhs.size() != rhs.size() || lhs.empty()) {
        return std::unexpected(RecognizerError::kInvalidArgument);
    }

    // Cosine similarity, matching the Rust core's matching metric. SeetaFace
    // embeddings are normalized, so for them this is equivalent to the dot
    // product; the division keeps it correct for unnormalized inputs too.
    const auto dot = std::transform_reduce(
        lhs.begin(), lhs.end(), rhs.begin(),
        0.0F, std::plus<>{},
        [](float left, float right) { return left * right; });
    const auto norm_lhs = std::sqrt(squared_norm(lhs));
    const auto norm_rhs = std::sqrt(squared_norm(rhs));
    if (norm_lhs <= std::numeric_limits<float>::epsilon()
        || norm_rhs <= std::numeric_limits<float>::epsilon()) {
        return 0.0F;
    }
    return std::clamp(dot / (norm_lhs * norm_rhs), -1.0F, 1.0F);
}

void RecognizerService::close_camera() {
    if (camera_) {
        camera_->release();
        camera_.reset();
    }
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

#if SU_HAS_SEETAFACE

std::expected<void, RecognizerError> RecognizerService::ensure_seetaface_backend() const {
    // Fast path: already initialized, no lock needed.
    if (seetaface_backend_) {
        return {};
    }

    std::lock_guard lock(*backend_mutex_);
    // Double-check after acquiring the lock so concurrent fast-path callers
    // do not block each other for the full model-load duration.
    if (seetaface_backend_) {
        return {};
    }

    auto paths = seetaface_model_paths(default_seetaface_model_dir());
    if (!paths) {
        return std::unexpected(paths.error());
    }
    auto backend = std::make_unique<SeetaFaceBackend>(*std::move(paths));
    if (!backend->available()) {
        return std::unexpected(RecognizerError::kModelUnavailable);
    }
    seetaface_backend_ = std::move(backend);
    return {};
}

#else

std::expected<void, RecognizerError> RecognizerService::ensure_seetaface_backend() const {
    return std::unexpected(RecognizerError::kModelUnavailable);
}

#endif

std::expected<RecognitionResult, RecognizerError> RecognizerService::extract_from_image(
    ImageView image, bool liveness_enabled) const {
#if SU_HAS_SEETAFACE
    if (auto ensured = ensure_seetaface_backend(); !ensured) {
        return std::unexpected(ensured.error());
    }
    auto result = seetaface_backend_->extract(image, liveness_enabled);
    if (!result) {
        return std::unexpected(result.error());
    }
    // No face detected is a normal outcome, not an error: return the result
    // with has_face=false so callers can show "no face" without treating it
    // as a detection failure.
    return result;
#else
    (void)image;
    (void)liveness_enabled;
    return std::unexpected(RecognizerError::kModelUnavailable);
#endif
}

std::expected<RecognitionResult, RecognizerError> RecognizerService::predict_liveness(
    ImageView image, bool liveness_enabled) const {
#if SU_HAS_SEETAFACE
    if (auto ensured = ensure_seetaface_backend(); !ensured) {
        return std::unexpected(ensured.error());
    }
    return seetaface_backend_->predict_liveness(image, liveness_enabled);
#else
    (void)image;
    (void)liveness_enabled;
    return std::unexpected(RecognizerError::kModelUnavailable);
#endif
}

bool RecognizerService::seetaface_available() const {
#if SU_HAS_SEETAFACE
    return ensure_seetaface_backend().has_value();
#else
    return false;
#endif
}

}  // namespace su::recognizer
