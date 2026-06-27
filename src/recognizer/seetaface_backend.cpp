#include "recognizer/seetaface_backend.h"

#include <algorithm>
#include <array>
#include <exception>
#include <utility>
#include <vector>

#if SU_HAS_SEETAFACE
#include <seeta/FaceAntiSpoofing.h>
#include <seeta/FaceDetector.h>
#include <seeta/FaceLandmarker.h>
#include <seeta/FaceRecognizer.h>
#endif

namespace su::recognizer {

namespace {

constexpr auto kModelFiles = std::array{
    "face_detector.csta",
    "face_landmarker_pts5.csta",
    "face_recognizer.csta",
    "fas_first.csta",
    "fas_second.csta",
};

}  // namespace

std::filesystem::path default_seetaface_model_dir() {
#ifdef SU_SEETAFACE_MODEL_DIR
    if (std::filesystem::is_directory(SU_SEETAFACE_MODEL_DIR)) {
        return std::filesystem::path(SU_SEETAFACE_MODEL_DIR);
    }
#endif

    auto path = std::filesystem::current_path();
    while (true) {
        const auto candidate = path / "FaceRecognizer" / "resources" / "models";
        if (std::filesystem::is_directory(candidate)) {
            return candidate;
        }
        if (!path.has_parent_path() || path == path.parent_path()) {
            break;
        }
        path = path.parent_path();
    }
    return std::filesystem::current_path() / "FaceRecognizer" / "resources" / "models";
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
    return image.width > 0
        && image.height > 0
        && (image.channels == 1 || image.channels == 3 || image.channels == 4)
        && !image.bytes.empty();
}

float liveness_score_from(
    const seeta::FaceAntiSpoofing::Status status,
    const std::pair<float, float> scores) {
    const auto reality = std::clamp(scores.second, 0.0F, 1.0F);
    switch (status) {
    case seeta::FaceAntiSpoofing::REAL:
        return reality;
    case seeta::FaceAntiSpoofing::SPOOF:
        return 0.0F;
    case seeta::FaceAntiSpoofing::FUZZY:
    case seeta::FaceAntiSpoofing::DETECTING:
        return reality * 0.5F;
    }
    return 0.0F;
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

    std::expected<RecognitionResult, RecognizerError> extract(const ImageView image) const {
        if (!available()) {
            return std::unexpected(RecognizerError::kModelUnavailable);
        }
        if (!valid_image(image)) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }

        auto owned_bytes = std::vector<unsigned char>(image.bytes.size());
        std::ranges::transform(
            image.bytes,
            owned_bytes.begin(),
            [](const std::byte value) {
                return static_cast<unsigned char>(value);
            });
        auto seeta_image = SeetaImageData{
            .width = image.width,
            .height = image.height,
            .channels = image.channels,
            .data = owned_bytes.data(),
        };

        const auto faces = detector_->detect(seeta_image);
        if (faces.size <= 0 || faces.data == nullptr) {
            return RecognitionResult{};
        }

        const auto face = faces.data[0].pos;
        const auto points = landmarker_->mark(seeta_image, face);
        auto feature = std::vector<float>(recognizer_->GetExtractFeatureSize());
        if (!recognizer_->Extract(seeta_image, points.data(), feature.data())) {
            return std::unexpected(RecognizerError::kModelUnavailable);
        }

        auto liveness_score = 0.0F;
        if (anti_spoofing_) {
            const auto liveness_status = anti_spoofing_->Predict(seeta_image, face, points.data());
            auto clarity = 0.0F;
            auto reality = 0.0F;
            anti_spoofing_->GetPreFrameScore(&clarity, &reality);
            liveness_score = liveness_score_from(liveness_status, {clarity, reality});
        }

        return RecognitionResult{
            .has_face = true,
            .face_box = FaceBox{
                .x = face.x,
                .y = face.y,
                .width = face.width,
                .height = face.height,
            },
            .feature = std::move(feature),
            .liveness_score = liveness_score,
        };
    }

    bool available() const {
        return detector_ && landmarker_ && recognizer_;
    }

    bool liveness_available() const {
        return static_cast<bool>(anti_spoofing_);
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
};

#else

class SeetaFaceBackend::Impl {
public:
    explicit Impl(const SeetaFaceModelPaths&) {}

    std::expected<RecognitionResult, RecognizerError> extract(ImageView) const {
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
    const ImageView image) const {
    return impl_->extract(image);
}

bool SeetaFaceBackend::available() const {
    return impl_->available();
}

bool SeetaFaceBackend::liveness_available() const {
    return impl_->liveness_available();
}

}  // namespace su::recognizer
