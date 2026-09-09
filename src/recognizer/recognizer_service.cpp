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

    camera_ = std::make_unique<V4L2Camera>();
    auto opened = camera_->open(camera_index);
    if (opened) {
        active_camera_ = camera_index;
        return {};
    }

    const auto error = opened.error();
    camera_.reset();
    return std::unexpected(error);
}

std::expected<PreviewFrame, RecognizerError> RecognizerService::capture_preview_frame() const {
    std::lock_guard lock(camera_mutex_);
    if (!active_camera_) {
        return std::unexpected(RecognizerError::kCameraUnavailable);
    }
    if (!camera_) {
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
    return PreviewFrame{
        .width = frame->width,
        .height = frame->height,
        .rgba_or_rgb = std::move(*rgb),
    };
}

std::expected<std::pair<PreviewFrame, RecognitionResult>, RecognizerError>
RecognizerService::capture_and_extract(bool liveness_enabled) const {
    auto preview = PreviewFrame{};
    {
        std::lock_guard lock(camera_mutex_);
        if (!active_camera_) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }

        if (!camera_) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }

        // Copy the mmap-backed frame while holding the camera lock. SeetaFace
        // runs after the copy, so camera control does not stay blocked during
        // model inference.
        auto frame = camera_->grab_frame();
        if (!frame) {
            return std::unexpected(frame.error());
        }
        auto rgb = v4l2_frame_to_rgb(*frame);
        if (!rgb) {
            return std::unexpected(rgb.error());
        }
        preview = PreviewFrame{
            .width = frame->width,
            .height = frame->height,
            .rgba_or_rgb = std::move(*rgb),
        };
    }
    auto result = extract_from_image(
        ImageView{
            .width = preview.width,
            .height = preview.height,
            .channels = 3,
            .bytes = std::span<const std::byte>(preview.rgba_or_rgb),
        },
        liveness_enabled);
    if (!result) {
        return std::unexpected(result.error());
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
    std::lock_guard lock(camera_mutex_);
    close_camera_unlocked();
}

void RecognizerService::close_camera_unlocked() {
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
    auto lock = std::lock_guard{seetaface_mutex_};
    if (seetaface_backend_) {
        return {};
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_seetaface_retry_) {
        return std::unexpected(RecognizerError::kModelUnavailable);
    }
    // A missing model directory is recoverable after package installation or
    // a mount becoming available. Retry lazily instead of caching failure for
    // the lifetime of the daemon.
    next_seetaface_retry_ = now + std::chrono::seconds{5};
    try {
        auto paths = seetaface_model_paths(default_seetaface_model_dir());
        if (paths) {
            auto backend = std::make_unique<SeetaFaceBackend>(*std::move(paths));
            if (backend->available()) {
                seetaface_backend_ = std::move(backend);
                next_seetaface_retry_ = {};
            }
        }
    } catch (...) {
        // Model constructors are third-party code; expose a safe unavailable
        // result and allow the bounded retry above to recover later.
    }
    if (seetaface_backend_) {
        return {};
    }
    return std::unexpected(RecognizerError::kModelUnavailable);
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

std::expected<void, RecognizerError> RecognizerService::reset_liveness() const {
#if SU_HAS_SEETAFACE
    if (auto ensured = ensure_seetaface_backend(); !ensured) {
        return std::unexpected(ensured.error());
    }
    if (!seetaface_backend_->liveness_available()) {
        return std::unexpected(RecognizerError::kModelUnavailable);
    }
    seetaface_backend_->reset_liveness();
    return {};
#else
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
