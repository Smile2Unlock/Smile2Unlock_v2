module;
#include "common/log/logger.h"
#include "recognizer/exposure/exposure_control.h"

module su.recognizer.service;
import std;
import su.recognizer.camera;
import su.recognizer.image;
import su.recognizer.backend;

namespace su::recognizer {

RecognizerService::RecognizerService()
    = default;

RecognizerService::~RecognizerService() = default;

std::vector<CameraInfo> RecognizerService::enumerate_cameras() const {
    const auto devices = enumerate_v4l2_cameras();
    auto cameras = std::vector<CameraInfo>{};
    cameras.reserve(devices.size());
    for (const auto& device : devices) {
        cameras.push_back(CameraInfo{.index = device.index, .name = device.name});
    }
    return cameras;
}

std::expected<void, RecognizerError> RecognizerService::open_camera(int camera_index) {
    std::lock_guard lock(camera_mutex_);
    close_camera_unlocked();
    ++camera_generation_;

    camera_ = std::make_unique<V4L2Camera>();
    auto opened = camera_->open(camera_index);
    if (opened) {
        active_camera_ = camera_index;
        exposure_ = {};
        exposure_.caps = camera_->exposure_capabilities();
        exposure_.loop = detail::make_exposure_state(exposure_.caps.knobs);
        return {};
    }

    const auto error = opened.error();
    camera_.reset();
    return std::unexpected(error);
}

std::expected<PreviewFrame, RecognizerError> RecognizerService::grab_rgb_frame_locked() const {
    if (!active_camera_ || !camera_) {
        return std::unexpected(RecognizerError::kCameraUnavailable);
    }
    auto frame = camera_->grab_frame();
    if (!frame) {
        return std::unexpected(frame.error());
    }
    auto rgb = v4l2_frame_to_rgb(*frame);
    if (!rgb) {
        return std::unexpected(rgb.error());
    }
    exposure_.frame_width = frame->width;
    exposure_.frame_height = frame->height;
    return PreviewFrame{
        .width = frame->width,
        .height = frame->height,
        .rgba_or_rgb = std::move(*rgb),
        .camera_generation = camera_generation_,
    };
}

std::expected<PreviewFrame, RecognizerError> RecognizerService::capture_preview_frame() const {
    std::lock_guard lock(camera_mutex_);
    return grab_rgb_frame_locked();
}

std::expected<std::pair<PreviewFrame, RecognitionResult>, RecognizerError>
RecognizerService::capture_and_extract(bool liveness_enabled) const {
    // Copy the mmap-backed frame while holding the camera lock. The face
    // backend runs after the copy, so camera control does not stay blocked during
    // model inference.
    auto grabbed = [&] {
        std::lock_guard lock(camera_mutex_);
        return grab_rgb_frame_locked();
    }();
    if (!grabbed) {
        return std::unexpected(grabbed.error());
    }
    auto preview = std::move(*grabbed);
    const auto view = ImageView{
        .width = preview.width,
        .height = preview.height,
        .channels = 3,
        .bytes = std::span<const std::byte>(preview.rgba_or_rgb),
    };
    auto result = extract_from_image(view, liveness_enabled);
    if (!result) {
        return std::unexpected(result.error());
    }
    // A brightness step changes what liveness sees frame to frame, so start
    // its window over (failure just means liveness is unavailable).
    if (observe_exposure(view, result->face_box, preview.camera_generation)) {
        (void)reset_liveness();
    }
    return std::make_pair(std::move(preview), std::move(*result));
}

std::expected<RecognitionResult, RecognizerError> RecognizerService::extract_features(
    bool liveness_enabled) const {
    auto captured = capture_and_extract(liveness_enabled);
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

    // Cosine similarity, matching the Rust core's matching metric. Backend
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
    std::lock_guard lock(camera_mutex_);
    close_camera_unlocked();
}

void RecognizerService::close_camera_unlocked() {
    if (camera_) {
        camera_->release();
        camera_.reset();
    }
    active_camera_.reset();
    exposure_ = {};
    ++camera_generation_;
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

std::expected<void, RecognizerError> RecognizerService::ensure_backend() const {
    auto lock = std::lock_guard{backend_mutex_};
    if (backend_) {
        return {};
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_backend_retry_) {
        return std::unexpected(RecognizerError::kModelUnavailable);
    }
    // A missing model directory is recoverable after package installation or
    // a mount becoming available. Retry lazily instead of caching failure for
    // the lifetime of the daemon.
    next_backend_retry_ = now + std::chrono::seconds{5};
    try {
        if (auto created = create_default_backend(); created) {
            backend_ = *std::move(created);
            next_backend_retry_ = {};
        }
    } catch (...) {
        // Model constructors are third-party code; expose a safe unavailable
        // result and allow the bounded retry above to recover later.
    }
    if (backend_) {
        return {};
    }
    return std::unexpected(RecognizerError::kModelUnavailable);
}

std::expected<RecognitionResult, RecognizerError> RecognizerService::extract_from_image(
    ImageView image, bool liveness_enabled) const {
    if (auto ensured = ensure_backend(); !ensured) {
        return std::unexpected(ensured.error());
    }
    // No face detected is a normal outcome, not an error: the result carries
    // has_face=false so callers can show "no face" without treating it as a
    // detection failure.
    return backend_->extract(image, liveness_enabled, backlight_compensation());
}

std::expected<RecognitionResult, RecognizerError> RecognizerService::predict_liveness(
    ImageView image, bool liveness_enabled) const {
    if (auto ensured = ensure_backend(); !ensured) {
        return std::unexpected(ensured.error());
    }
    return backend_->predict_liveness(image, liveness_enabled, backlight_compensation());
}

std::expected<void, RecognizerError> RecognizerService::reset_liveness() const {
    if (auto ensured = ensure_backend(); !ensured) {
        return std::unexpected(ensured.error());
    }
    if (!backend_->liveness_available()) {
        return std::unexpected(RecognizerError::kModelUnavailable);
    }
    backend_->reset_liveness();
    return {};
}

bool RecognizerService::backend_available() const {
    return ensure_backend().has_value();
}

std::string RecognizerService::backend_name() const {
    if (!ensure_backend()) {
        return {};
    }
    return std::string{backend_->name()};
}

}  // namespace su::recognizer
