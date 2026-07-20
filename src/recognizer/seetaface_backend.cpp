module;
#if SU_HAS_SEETAFACE
#include <seeta/FaceAntiSpoofing.h>
#include <seeta/FaceDetector.h>
#include <seeta/FaceLandmarker.h>
#include <seeta/FaceRecognizer.h>
#endif
#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <print>
#include <utility>
#include <vector>

module su.recognizer.backend;
import su.recognizer.types;

namespace su::recognizer {

namespace {

constexpr auto kModelFiles = std::array{
    "face_detector.csta",
    "face_landmarker_pts5.csta",
    "face_recognizer.csta",
    "fas_first.csta",
    "fas_second.csta",
};

std::optional<std::filesystem::path> find_model_dir_from(const std::filesystem::path& start) {
    auto path = start;
    while (true) {
        for (const auto& candidate : {
                 path / "assets" / "models" / "seeta",
                 path / "FaceRecognizer" / "resources" / "models",
             }) {
            auto error = std::error_code{};
            if (std::filesystem::is_directory(candidate, error)) {
                return candidate;
            }
        }
        if (!path.has_parent_path() || path == path.parent_path()) {
            break;
        }
        path = path.parent_path();
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> installed_model_dir() {
#if defined(__linux__)
    constexpr auto path = std::string_view{"/usr/share/smile2unlock/models"};
    auto error = std::error_code{};
    if (std::filesystem::is_directory(path, error)) {
        return std::filesystem::path(path);
    }
#endif
    return std::nullopt;
}

}  // namespace

std::filesystem::path default_seetaface_model_dir() {
    auto error = std::error_code{};
    if (const auto* configured = std::getenv("SU_SEETAFACE_MODEL_DIR");
        configured != nullptr && std::filesystem::is_directory(configured, error)) {
        return std::filesystem::path(configured);
    }
#ifdef SU_SEETAFACE_MODEL_DIR
    error.clear();
    if (std::filesystem::is_directory(SU_SEETAFACE_MODEL_DIR, error)) {
        return std::filesystem::path(SU_SEETAFACE_MODEL_DIR);
    }
#endif

    if (const auto model_dir = find_model_dir_from(std::filesystem::current_path())) {
        return *model_dir;
    }
    if (const auto model_dir = installed_model_dir()) {
        return *model_dir;
    }
    return std::filesystem::current_path() / "assets" / "models" / "seeta";
}

std::expected<SeetaFaceModelPaths, RecognizerError> seetaface_model_paths(
    const std::filesystem::path& model_dir) {
    const auto paths = SeetaFaceModelPaths{
        .detector = model_dir / kModelFiles[0],
        .landmarker = model_dir / kModelFiles[1],
        .recognizer = model_dir / kModelFiles[2],
        .anti_spoofing_first = model_dir / kModelFiles[3],
        .anti_spoofing_second = model_dir / kModelFiles[4],
    };
    if (!std::filesystem::is_regular_file(paths.detector)
        || !std::filesystem::is_regular_file(paths.landmarker)
        || !std::filesystem::is_regular_file(paths.recognizer)
        || !std::filesystem::is_regular_file(paths.anti_spoofing_first)
        || !std::filesystem::is_regular_file(paths.anti_spoofing_second)) {
        return std::unexpected(RecognizerError::kModelUnavailable);
    }
    return paths;
}

#if SU_HAS_SEETAFACE

namespace {

bool valid_image(const ImageView image) {
    if (image.width <= 0 || image.height <= 0
        || (image.channels != 1 && image.channels != 3 && image.channels != 4)) {
        return false;
    }
    const auto width = static_cast<std::size_t>(image.width);
    const auto height = static_cast<std::size_t>(image.height);
    const auto channels = static_cast<std::size_t>(image.channels);
    if (width > std::numeric_limits<std::size_t>::max() / height
        || width * height > std::numeric_limits<std::size_t>::max() / channels) {
        return false;
    }
    return image.bytes.size() >= width * height * channels;
}

float liveness_score_from(
    const seeta::FaceAntiSpoofing::Status status,
    const std::pair<float, float> scores) {
    const auto reality = std::clamp(scores.second, 0.0F, 1.0F);
    switch (status) {
    case seeta::FaceAntiSpoofing::REAL:
        return reality;
    case seeta::FaceAntiSpoofing::SPOOF:
    case seeta::FaceAntiSpoofing::FUZZY:
    case seeta::FaceAntiSpoofing::DETECTING:
        return 0.0F;
    }
    std::unreachable();
}

// Human-readable label for the anti-spoofing status. Logged on status
// transitions; compiles to a constant string in release builds.
const char* liveness_status_name(seeta::FaceAntiSpoofing::Status status) {
    switch (status) {
    case seeta::FaceAntiSpoofing::REAL: return "REAL";
    case seeta::FaceAntiSpoofing::SPOOF: return "SPOOF";
    case seeta::FaceAntiSpoofing::FUZZY: return "FUZZY";
    case seeta::FaceAntiSpoofing::DETECTING: return "DETECTING";
    }
    std::unreachable();
}

}  // namespace

class SeetaFaceBackend::Impl {
public:
    explicit Impl(const SeetaFaceModelPaths& paths)
        : paths_(paths) {
        try {
            detector_ = std::make_unique<seeta::FaceDetector>(setting_for(paths_.detector));
            landmarker_ = std::make_unique<seeta::FaceLandmarker>(setting_for(paths_.landmarker));
            recognizer_ = std::make_unique<seeta::FaceRecognizer>(setting_for(paths_.recognizer));
        } catch (const std::exception&) {
            detector_.reset();
            landmarker_.reset();
            recognizer_.reset();
        } catch (...) {
            detector_.reset();
            landmarker_.reset();
            recognizer_.reset();
        }

        try {
            anti_spoofing_ = std::make_unique<seeta::FaceAntiSpoofing>(anti_spoofing_setting_for(paths_));
            anti_spoofing_->SetThreshold(0.3F, 0.8F);
        } catch (const std::exception& error) {
            anti_spoofing_.reset();
        } catch (...) {
            anti_spoofing_.reset();
        }
    }

    // Detect the primary face, run landmark detection and, if anti-spoofing is
    // loaded, run PredictVideo. FaceAntiSpoofing is a stateful video-stream model:
    // it must be fed on consecutive frames to reach a stable REAL/SPOOF verdict,
    // so this is called once per preview frame. Returns has_face + face_box +
    // liveness_score. When extract_feature is set, also runs feature extraction
    // (used on the throttled detect cadence). ResetVideo is called at each new
    // preview/auth session. The mutex serializes access from
    // the preview thread (predict_liveness) and the detect thread (extract).
    std::expected<RecognitionResult, RecognizerError> run_pipeline(
        const ImageView image, bool extract_feature, bool liveness_enabled) const {
        std::lock_guard lock(mutex_);
        if (!available()) {
            return std::unexpected(RecognizerError::kModelUnavailable);
        }
        if (!valid_image(image)) {
            return std::unexpected(RecognizerError::kInvalidImage);
        }

        // Zero-overhead copy: std::byte and unsigned char share the same
        // representation on all supported platforms; a single memcpy replaces
        // the per-element transform/static_cast chain.
        auto owned_bytes = std::vector<unsigned char>(image.bytes.size());
        if (!image.bytes.empty()) {
            std::memcpy(owned_bytes.data(), image.bytes.data(), image.bytes.size_bytes());
        }
        auto seeta_image = SeetaImageData{
            .width = image.width,
            .height = image.height,
            .channels = image.channels,
            .data = owned_bytes.data(),
        };

        const auto faces = detector_->detect(seeta_image);
        if (faces.size <= 0 || faces.data == nullptr) {
            // Still feed anti-spoofing? No: it needs a face box/points. A frame
            // with no face resets the model's expectation implicitly via the
            // absence of Predict; report no-face and let the caller decide.
            return RecognitionResult{};
        }
        if (faces.size != 1) {
            // Authentication must not silently choose one identity from a
            // multi-person frame. Require the user to present alone.
            return RecognitionResult{};
        }

        const auto face = faces.data[0].pos;
        const auto points = landmarker_->mark(seeta_image, face);

        // Liveness is optional per-call: when disabled in config the caller
        // skips anti-spoofing entirely (Predict is the expensive part of the
        // pipeline beyond detect+landmark), returns a passing score so the
        // frame still has a face box without a liveness gate.
        auto liveness_score = liveness_enabled ? 0.0F : 1.0F;
        if (liveness_enabled && anti_spoofing_) {
            const auto liveness_status = anti_spoofing_->PredictVideo(
                seeta_image, face, points.data());
            auto clarity = 0.0F;
            auto reality = 0.0F;
            anti_spoofing_->GetPreFrameScore(&clarity, &reality);
            liveness_score = liveness_score_from(liveness_status, {clarity, reality});
            // Log only on status transitions to keep stderr quiet during
            // stable REAL/SPOOF stretches (which may last hundreds of frames).
            if (liveness_status != prev_liveness_status_) {
                prev_liveness_status_ = liveness_status;
                std::println(stderr, "[seeta] liveness status={} clarity={:.3f} reality={:.3f} score={:.3f}",
                             liveness_status_name(liveness_status), clarity, reality, liveness_score);
            }
        }

        RecognitionResult result{
            .has_face = true,
            .face_box = FaceBox{
                .x = face.x,
                .y = face.y,
                .width = face.width,
                .height = face.height,
            },
            .feature = {},
            .liveness_score = liveness_score,
        };

        if (extract_feature) {
            auto feature = std::vector<float>(recognizer_->GetExtractFeatureSize());
            if (!recognizer_->Extract(seeta_image, points.data(), feature.data())) {
                return std::unexpected(RecognizerError::kModelUnavailable);
            }
            result.feature = std::move(feature);
        }
        return result;
    }

    std::expected<RecognitionResult, RecognizerError> extract(
        const ImageView image, bool liveness_enabled) const {
        return run_pipeline(image, /*extract_feature=*/true, liveness_enabled);
    }

    std::expected<RecognitionResult, RecognizerError> predict_liveness(
        const ImageView image, bool liveness_enabled) const {
        return run_pipeline(image, /*extract_feature=*/false, liveness_enabled);
    }

    bool available() const {
        return detector_ && landmarker_ && recognizer_;
    }

    bool liveness_available() const {
        return static_cast<bool>(anti_spoofing_);
    }

    void reset_liveness() {
        std::lock_guard lock(mutex_);
        if (anti_spoofing_) {
            anti_spoofing_->ResetVideo();
        }
        prev_liveness_status_ = seeta::FaceAntiSpoofing::DETECTING;
    }

private:
    static seeta::ModelSetting setting_for(const std::filesystem::path& path) {
        auto setting = seeta::ModelSetting{};
        setting.append(path.string());
        return setting;
    }

    static seeta::ModelSetting anti_spoofing_setting_for(const SeetaFaceModelPaths& paths) {
        auto setting = seeta::ModelSetting{};
        setting.append(paths.anti_spoofing_first.string());
        setting.append(paths.anti_spoofing_second.string());
        return setting;
    }

    SeetaFaceModelPaths paths_;
    std::unique_ptr<seeta::FaceDetector> detector_;
    std::unique_ptr<seeta::FaceLandmarker> landmarker_;
    std::unique_ptr<seeta::FaceRecognizer> recognizer_;
    std::unique_ptr<seeta::FaceAntiSpoofing> anti_spoofing_;
    mutable std::mutex mutex_;
    mutable seeta::FaceAntiSpoofing::Status prev_liveness_status_ = seeta::FaceAntiSpoofing::DETECTING;
};

#else

class SeetaFaceBackend::Impl {
public:
    explicit Impl(const SeetaFaceModelPaths&) {}

    std::expected<RecognitionResult, RecognizerError> extract(ImageView, bool) const {
        return std::unexpected(RecognizerError::kModelUnavailable);
    }

    bool available() const {
        return false;
    }

    bool liveness_available() const {
        return false;
    }
};

#endif

SeetaFaceBackend::SeetaFaceBackend(SeetaFaceModelPaths paths)
    : impl_(std::make_unique<Impl>(paths)) {}

SeetaFaceBackend::~SeetaFaceBackend() = default;

SeetaFaceBackend::SeetaFaceBackend(SeetaFaceBackend&&) noexcept = default;

SeetaFaceBackend& SeetaFaceBackend::operator=(SeetaFaceBackend&&) noexcept = default;

std::expected<RecognitionResult, RecognizerError> SeetaFaceBackend::extract(
    const ImageView image, bool liveness_enabled) const {
    return impl_->extract(image, liveness_enabled);
}

std::expected<RecognitionResult, RecognizerError> SeetaFaceBackend::predict_liveness(
    const ImageView image, bool liveness_enabled) const {
#if SU_HAS_SEETAFACE
    return impl_->predict_liveness(image, liveness_enabled);
#else
    (void)image;
    (void)liveness_enabled;
    return std::unexpected(RecognizerError::kModelUnavailable);
#endif
}

bool SeetaFaceBackend::available() const {
    return impl_->available();
}

bool SeetaFaceBackend::liveness_available() const {
    return impl_->liveness_available();
}

void SeetaFaceBackend::reset_liveness() {
#if SU_HAS_SEETAFACE
    impl_->reset_liveness();
#endif
}

}  // namespace su::recognizer
