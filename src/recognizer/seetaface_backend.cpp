module;
#include "common/log/logger.h"
#include <format>
#include "liveness_window.h"
#include "recognizer/exposure/low_light_enhance.h"
#include "common/utf8_path.h"
#if defined(_WIN32)
#include "platform/windows/paths.h"
#endif
#if SU_HAS_SEETAFACE
#include <seeta/FaceAntiSpoofing.h>
#include <seeta/FaceDetector.h>
#include <seeta/FaceLandmarker.h>
#include <seeta/FaceRecognizer.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <mdspan>
#include <mutex>
#include <optional>
#include <print>
#include <span>
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
#if defined(_WIN32)
    if (const auto configured = su::windows::environment_path(L"SU_SEETAFACE_MODEL_DIR");
        configured && std::filesystem::is_directory(*configured, error)) {
        return *configured;
    }
    if (const auto model_dir = find_model_dir_from(su::windows::executable_path().parent_path())) {
        return *model_dir;
    }
#else
    if (const auto* configured = std::getenv("SU_SEETAFACE_MODEL_DIR");
        configured != nullptr && std::filesystem::is_directory(configured, error)) {
        return std::filesystem::path(configured);
    }
#endif
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
#if defined(_WIN32)
    return su::windows::executable_path().parent_path() / L"assets" / L"models" / L"seeta";
#else
    return std::filesystem::current_path() / "assets" / "models" / "seeta";
#endif
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

        auto model_error = std::error_code{};
        if (!std::filesystem::is_regular_file(paths_.anti_spoofing_first, model_error)
            || !std::filesystem::is_regular_file(paths_.anti_spoofing_second, model_error)) {
            return;
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
    // loaded, run Predict and aggregate consecutive frame scores locally:
    // the video policy needs ten frames to reach a stable REAL/SPOOF verdict,
    // so this is called once per preview frame. Returns has_face + face_box +
    // liveness_score. When extract_feature is set, also runs feature extraction
    // (used on the throttled detect cadence). The window is reset at each new
    // preview/auth session. The mutex serializes access from
    // the preview thread (predict_liveness) and the detect thread (extract).
    std::expected<RecognitionResult, RecognizerError> run_pipeline(
        const ImageView image, bool extract_feature, bool liveness_enabled,
        bool low_light_retry) const {
        std::lock_guard lock(mutex_);
        if (!available()) {
            return std::unexpected(RecognizerError::kModelUnavailable);
        }
        if (!valid_image(image)) {
            reset_liveness_locked();
            return std::unexpected(RecognizerError::kInvalidImage);
        }
        if (liveness_enabled && !anti_spoofing_) {
            return std::unexpected(RecognizerError::kModelUnavailable);
        }
        if (!liveness_enabled) {
            reset_liveness_locked();
        }
        const auto bgr = to_bgr(image);
        if (!bgr) {
            reset_liveness_locked();
            return std::unexpected(RecognizerError::kInvalidImage);
        }
        const auto seeta_image = SeetaImageData{
            .width = image.width,
            .height = image.height,
            .channels = 3,
            .data = const_cast<unsigned char*>(bgr->data()),
        };

        const auto detection = detect_primary_face(seeta_image, *bgr, low_light_retry);
        if (!detection) {
            reset_liveness_locked();
            return RecognitionResult{};
        }
        if (detection->enhanced_only && !face_bright_enough(seeta_image, detection->face)) {
            // Metering-only: the box lets the exposure loop brighten the
            // scene, but nothing downstream may see this near-black frame.
            reset_liveness_locked();
            return RecognitionResult{.has_face = false, .face_box = to_face_box(detection->face)};
        }
        return recognize_face(seeta_image, detection->face, extract_feature, liveness_enabled);
    }

    std::expected<RecognitionResult, RecognizerError> extract(
        const ImageView image, bool liveness_enabled, bool low_light_retry) const {
        return run_pipeline(image, /*extract_feature=*/true, liveness_enabled, low_light_retry);
    }

    std::expected<RecognitionResult, RecognizerError> predict_liveness(
        const ImageView image, bool liveness_enabled, bool low_light_retry) const {
        return run_pipeline(image, /*extract_feature=*/false, liveness_enabled, low_light_retry);
    }

    bool available() const {
        return detector_ && landmarker_ && recognizer_;
    }

    bool liveness_available() const {
        return static_cast<bool>(anti_spoofing_);
    }

    void reset_liveness() {
        std::lock_guard lock(mutex_);
        reset_liveness_locked();
    }

private:
    // Detector input: the original image, or a copy box-downscaled to
    // kDetectMaxWidth. `image.data` points into `storage`, so keep the
    // struct alive (and do not copy it) while `image` is in use.
    struct DetectInput {
        std::vector<unsigned char> storage;
        SeetaImageData image{};
        double scale_x = 1.0;
        double scale_y = 1.0;
    };

    // Detection cost scales with pixel count, and high-resolution cameras
    // (1080p+) otherwise push the preview under 1 fps. The detector only
    // needs to locate the face, so cap the width at ~640px.
    [[nodiscard]] static DetectInput downscale_for_detect(
        const SeetaImageData& source, const std::vector<unsigned char>& source_bytes) {
        constexpr int kDetectMaxWidth = 640;
        auto input = DetectInput{.image = source};
        if (source.width <= kDetectMaxWidth) {
            return input;
        }
        const auto scaled_width = kDetectMaxWidth;
        const auto scaled_height = std::max(1, source.height * kDetectMaxWidth / source.width);
        input.storage.assign(static_cast<std::size_t>(scaled_width) * scaled_height * 3, 0);
        // Integer box filter: average each source block into one pixel.
        using Pixels = std::dextents<std::size_t, 3>;  // rows x columns x BGR
        const auto src = std::mdspan<const unsigned char, Pixels>(source_bytes.data(),
            static_cast<std::size_t>(source.height), static_cast<std::size_t>(source.width), 3);
        auto* dst = input.storage.data();
        for (int y = 0; y < scaled_height; ++y) {
            const auto y0 = source.height * y / scaled_height;
            const auto y1 = std::max(y0 + 1, source.height * (y + 1) / scaled_height);
            for (int x = 0; x < scaled_width; ++x) {
                const auto x0 = source.width * x / scaled_width;
                const auto x1 = std::max(x0 + 1, source.width * (x + 1) / scaled_width);
                auto r = 0UL;
                auto g = 0UL;
                auto b = 0UL;
                auto count = 0UL;
                for (int sy = y0; sy < y1; ++sy) {
                    for (int sx = x0; sx < x1; ++sx) {
                        r += src[sy, sx, 0];
                        g += src[sy, sx, 1];
                        b += src[sy, sx, 2];
                        ++count;
                    }
                }
                *dst++ = static_cast<unsigned char>(r / count);
                *dst++ = static_cast<unsigned char>(g / count);
                *dst++ = static_cast<unsigned char>(b / count);
            }
        }
        input.image = SeetaImageData{
            .width = scaled_width,
            .height = scaled_height,
            .channels = 3,
            .data = input.storage.data(),
        };
        input.scale_x = static_cast<double>(source.width) / scaled_width;
        input.scale_y = static_cast<double>(source.height) / scaled_height;
        return input;
    }

    // The single face found by the detection stage, in full-resolution
    // coordinates. `enhanced_only` is set when the plain pass missed it and
    // only the low-light retry found it.
    struct PrimaryFace {
        SeetaRect face{};
        bool enhanced_only = false;
    };

    struct Detected {
        SeetaFaceInfoArray faces{};
        bool enhanced = false;
    };

    // ImageView is RGB/RGBA/gray, but every SeetaFace model expects BGR.
    // Swap red and blue only at the SDK boundary; preview stays RGB.
    [[nodiscard]] static std::optional<std::vector<unsigned char>> to_bgr(const ImageView image) {
        const auto pixels = static_cast<std::size_t>(image.width) * image.height;
        if (pixels > std::numeric_limits<std::size_t>::max() / 3) {
            return std::nullopt;
        }
        auto bgr = std::vector<unsigned char>(pixels * 3);
        for (auto i = std::size_t{}; i < pixels; ++i) {
            for (auto channel = 0; channel < 3; ++channel) {
                bgr[i * 3 + channel] = std::to_integer<unsigned char>(
                    image.bytes[i * image.channels + (image.channels == 1 ? 0 : 2 - channel)]);
            }
        }
        return bgr;
    }

    [[nodiscard]] static FaceBox to_face_box(const SeetaRect& rect) {
        return FaceBox{.x = rect.x, .y = rect.y, .width = rect.width, .height = rect.height};
    }

    // Detects on a downscaled copy and maps the box back to the full
    // resolution so landmarking, anti-spoofing and embedding keep their
    // accuracy on the original image. Authentication must not silently choose
    // one identity from a multi-person frame, so anything but exactly one face
    // yields nullopt.
    [[nodiscard]] std::optional<PrimaryFace> detect_primary_face(
        const SeetaImageData& image, const std::vector<unsigned char>& bgr,
        const bool low_light_retry) const {
        const auto detect_input = downscale_for_detect(image, bgr);
        const auto detected = detect_with_low_light_retry(detect_input.image, low_light_retry);
        if (detected.faces.size != 1 || detected.faces.data == nullptr) {
            return std::nullopt;
        }
        const auto scaled_face = detected.faces.data[0].pos;
        return PrimaryFace{
            .face = SeetaRect{
                .x = static_cast<int32_t>(scaled_face.x * detect_input.scale_x),
                .y = static_cast<int32_t>(scaled_face.y * detect_input.scale_y),
                .width = static_cast<int32_t>(scaled_face.width * detect_input.scale_x),
                .height = static_cast<int32_t>(scaled_face.height * detect_input.scale_y),
            },
            .enhanced_only = detected.enhanced,
        };
    }

    // Mean luma of the inner face region on the ORIGINAL (un-enhanced) image.
    [[nodiscard]] static bool face_bright_enough(const SeetaImageData& image, const SeetaRect& face) {
        const auto sample_count = static_cast<std::size_t>(image.width)
            * static_cast<std::size_t>(image.height) * 3;
        const auto pixels = std::span<const unsigned char>(image.data, sample_count);
        const auto region = detail::metering_region(image.width, image.height,
            detail::PixelRegion{.x = face.x, .y = face.y, .width = face.width, .height = face.height});
        return detail::enhanced_face_is_usable(detail::measure_luma(std::as_bytes(pixels),
            image.width, image.height, 3, region, detail::PixelOrder::bgr));
    }

    // Landmarks, liveness and (optionally) the embedding for a trusted face.
    std::expected<RecognitionResult, RecognizerError> recognize_face(
        const SeetaImageData& image, const SeetaRect& face, const bool extract_feature,
        const bool liveness_enabled) const {
        const auto points = landmarker_->mark(image, face);
        auto result = RecognitionResult{
            .has_face = true,
            .face_box = to_face_box(face),
            .feature = {},
            .liveness_score = score_liveness(image, face, points, liveness_enabled),
        };
        if (extract_feature) {
            auto feature = std::vector<float>(recognizer_->GetExtractFeatureSize());
            if (!recognizer_->Extract(image, points.data(), feature.data())) {
                return std::unexpected(RecognizerError::kModelUnavailable);
            }
            result.feature = std::move(feature);
        }
        return result;
    }

    // Liveness is optional per-call: when disabled in config the caller skips
    // anti-spoofing entirely (Predict is the expensive part of the pipeline
    // beyond detect+landmark) and gets a passing score, so the frame still has
    // a face box without a liveness gate.
    float score_liveness(const SeetaImageData& image, const SeetaRect& face,
        const std::vector<SeetaPointF>& points, const bool liveness_enabled) const {
        if (!liveness_enabled) {
            return 1.0F;
        }
        const auto liveness_status = anti_spoofing_->Predict(image, face, points.data());
        auto clarity = 0.0F;
        auto reality = 0.0F;
        anti_spoofing_->GetPreFrameScore(&clarity, &reality);
        const auto liveness_score = liveness_window_.push(clarity, reality);
        const auto now = std::chrono::steady_clock::now();
        const auto passes = liveness_score > 0.0F;
        // Include the local window's verdict and progress, not only the
        // SDK's single-frame status (REAL can still be warming up).
        if (liveness_status != prev_liveness_status_
            || passes != prev_liveness_passes_ || now >= next_liveness_log_) {
            prev_liveness_status_ = liveness_status;
            prev_liveness_passes_ = passes;
            next_liveness_log_ = now + std::chrono::seconds{3};
            su::log::process_logger().debug("seeta.liveness", {
                su::log::field("status", liveness_status_name(liveness_status)),
                su::log::field("clarity", clarity), su::log::field("reality", reality),
                su::log::field("score", liveness_score),
                su::log::field("frames", liveness_window_.sample_count())});
        }
        return liveness_score;
    }

    // Backlit faces can be too dark for the detector (#137). Only when the
    // plain pass finds nothing, retry on a gamma-brightened copy so the
    // exposure loop gets a face box to meter. The enhanced copy is used for
    // detection alone; landmarks, liveness and the embedding stay on the
    // untouched original so templates and liveness scores do not shift.
    Detected detect_with_low_light_retry(
        const SeetaImageData& detect_image, const bool low_light_retry) const {
        const auto first = detector_->detect(detect_image);
        // With the retry off (user switch) the first pass is final, exactly as
        // before the retry existed.
        if (!low_light_retry || (first.size > 0 && first.data != nullptr)) {
            return {.faces = first};
        }
        const auto sample_count = static_cast<std::size_t>(detect_image.width)
            * static_cast<std::size_t>(detect_image.height) * 3;
        const auto pixels = std::span<const unsigned char>(detect_image.data, sample_count);
        // The detector input is BGR (SeetaFace's channel order), not RGB.
        const auto centre = detail::measure_luma(std::as_bytes(pixels), detect_image.width,
            detect_image.height, 3,
            detail::metering_region(detect_image.width, detect_image.height, std::nullopt),
            detail::PixelOrder::bgr);
        return detail::low_light_lut(centre)
            .transform([&](const detail::LowLightLut& lut) {
                // Reused every frame: the dark, face-less case is the steady
                // state while the exposure loop is still adjusting.
                enhance_scratch_.resize(sample_count);
                detail::apply_lut(pixels, lut, enhance_scratch_);
                auto enhanced_image = detect_image;
                enhanced_image.data = enhance_scratch_.data();
                const auto retried = detector_->detect(enhanced_image);
                log_low_light_detect(centre.mean, retried.size);
                return Detected{.faces = retried, .enhanced = retried.size > 0};
            })
            .value_or(Detected{.faces = first});
    }

    void log_low_light_detect(const double centre_mean, const int faces_found) const {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_low_light_log_) {
            return;
        }
        next_low_light_log_ = now + std::chrono::seconds{3};
        su::log::process_logger().debug("seeta.low_light_detect", {
            su::log::field("centre_mean", centre_mean),
            su::log::field("outcome", faces_found > 0 ? "found" : "none"),
            su::log::field("faces", faces_found)});
    }

    void reset_liveness_locked() const {
        liveness_window_.reset();
        if (anti_spoofing_) {
            anti_spoofing_->ResetVideo();
        }
        prev_liveness_status_ = seeta::FaceAntiSpoofing::DETECTING;
        prev_liveness_passes_ = false;
        next_liveness_log_ = {};
    }

    static seeta::ModelSetting setting_for(const std::filesystem::path& path) {
        auto setting = seeta::ModelSetting{};
        setting.append(su::path_utf8(path));
        return setting;
    }

    static seeta::ModelSetting anti_spoofing_setting_for(const SeetaFaceModelPaths& paths) {
        auto setting = seeta::ModelSetting{};
        setting.append(su::path_utf8(paths.anti_spoofing_first));
        setting.append(su::path_utf8(paths.anti_spoofing_second));
        return setting;
    }

    SeetaFaceModelPaths paths_;
    std::unique_ptr<seeta::FaceDetector> detector_;
    std::unique_ptr<seeta::FaceLandmarker> landmarker_;
    std::unique_ptr<seeta::FaceRecognizer> recognizer_;
    std::unique_ptr<seeta::FaceAntiSpoofing> anti_spoofing_;
    mutable std::mutex mutex_;
    // Gamma-enhanced detector input, reused across frames. Guarded by mutex_.
    mutable std::vector<unsigned char> enhance_scratch_;
    mutable detail::LivenessWindow liveness_window_;
    mutable seeta::FaceAntiSpoofing::Status prev_liveness_status_ = seeta::FaceAntiSpoofing::DETECTING;
    mutable bool prev_liveness_passes_ = false;
    mutable std::chrono::steady_clock::time_point next_liveness_log_{};
    mutable std::chrono::steady_clock::time_point next_low_light_log_{};
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
    const ImageView image, bool liveness_enabled, bool low_light_retry) const {
    return impl_->extract(image, liveness_enabled, low_light_retry);
}

std::expected<RecognitionResult, RecognizerError> SeetaFaceBackend::predict_liveness(
    const ImageView image, bool liveness_enabled, bool low_light_retry) const {
#if SU_HAS_SEETAFACE
    return impl_->predict_liveness(image, liveness_enabled, low_light_retry);
#else
    (void)image;
    (void)liveness_enabled;
    (void)low_light_retry;
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
