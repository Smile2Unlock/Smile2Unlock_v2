module;
#include "common/log/logger.h"
#include "recognizer/exposure/low_light_enhance.h"
#include "recognizer/lightface_preprocess.h"
#if defined(_WIN32)
#include "platform/windows/paths.h"
#endif
#if SU_HAS_LIGHTFACE
#if defined(__MINGW32__) && !defined(_stdcall)
#define SU_DEFINED_MINGW_STDCALL 1
#define _stdcall __stdcall
#endif
#include <onnxruntime_cxx_api.h>
#ifdef SU_DEFINED_MINGW_STDCALL
#undef _stdcall
#undef SU_DEFINED_MINGW_STDCALL
#endif
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module su.recognizer.backend;
import su.recognizer.types;

namespace su::recognizer {

namespace {

constexpr auto kDetectorFile = "detector_5pt_lcf.onnx";
constexpr auto kRecognizerFile = "recognizer_edgeface.onnx";
constexpr auto kFasSpatialFile = "fas_spatial_enc.onnx";
constexpr auto kFasTemporalFile = "fas_temporal_head.onnx";

bool is_file(const std::filesystem::path& path) {
    auto error = std::error_code{};
    return !path.empty() && std::filesystem::is_regular_file(path, error);
}

bool is_dir(const std::filesystem::path& path) {
    auto error = std::error_code{};
    return !path.empty() && std::filesystem::is_directory(path, error);
}

std::optional<std::filesystem::path> environment_dir(const char* name) {
#if defined(_WIN32)
    const auto wide = std::wstring(name, name + std::char_traits<char>::length(name));
    return su::windows::environment_path(wide.c_str());
#else
    const auto* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path(value);
#endif
}

std::optional<std::filesystem::path> find_model_dir_from(const std::filesystem::path& start) {
    auto path = start;
    while (true) {
        const auto candidate = path / "assets" / "models" / "lightface";
        if (is_dir(candidate)) {
            return candidate;
        }
        if (!path.has_parent_path() || path == path.parent_path()) {
            break;
        }
        path = path.parent_path();
    }
    return std::nullopt;
}

}  // namespace

std::filesystem::path default_lightface_model_dir() {
    if (const auto configured = environment_dir("SU_LIGHTFACE_MODEL_DIR"); configured && is_dir(*configured)) {
        return *configured;
    }
    // SU_MODEL_DIR is the parent shared by every backend's model set.
    if (const auto base = environment_dir("SU_MODEL_DIR")) {
        for (const auto& candidate : {*base / "lightface", *base}) {
            if (is_file(candidate / kDetectorFile)) {
                return candidate;
            }
        }
    }
#ifdef SU_LIGHTFACE_MODEL_DIR
    if (is_dir(SU_LIGHTFACE_MODEL_DIR)) {
        return std::filesystem::path(SU_LIGHTFACE_MODEL_DIR);
    }
#endif
#if defined(_WIN32)
    if (const auto found = find_model_dir_from(su::windows::executable_path().parent_path())) {
        return *found;
    }
#else
    if (const auto found = find_model_dir_from(std::filesystem::current_path())) {
        return *found;
    }
#endif
#if defined(__linux__)
    for (const auto* installed : {"/usr/share/smile2unlock/models/lightface", "/usr/share/smile2unlock/models"}) {
        if (is_file(std::filesystem::path(installed) / kDetectorFile)) {
            return std::filesystem::path(installed);
        }
    }
#endif
#if defined(_WIN32)
    return su::windows::executable_path().parent_path() / L"assets" / L"models" / L"lightface";
#else
    return std::filesystem::current_path() / "assets" / "models" / "lightface";
#endif
}

std::expected<LightFaceModelPaths, RecognizerError> lightface_model_paths(
    const std::filesystem::path& model_dir) {
    auto paths = LightFaceModelPaths{
        .detector = model_dir / kDetectorFile,
        .recognizer = model_dir / kRecognizerFile,
        .fas_spatial = model_dir / kFasSpatialFile,
        .fas_temporal = model_dir / kFasTemporalFile,
    };
    if (!is_file(paths.detector) || !is_file(paths.recognizer)) {
        return std::unexpected(RecognizerError::kModelUnavailable);
    }
    return paths;
}

#if SU_HAS_LIGHTFACE

namespace {

namespace lf = su::recognizer::lightface;

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

// The detector, recognizer and liveness models share one process-wide
// environment. Intentionally leaked: tearing it down during static
// destruction races with the runtime library unloading.
Ort::Env& shared_environment() {
    static auto* environment = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "smile2unlock");
    return *environment;
}

Ort::SessionOptions make_session_options() {
    auto options = Ort::SessionOptions{};
    const auto threads = std::clamp<int>(static_cast<int>(std::thread::hardware_concurrency()), 1, 4);
    options.SetIntraOpNumThreads(threads);
    options.SetInterOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    // Idle worker threads must not spin between frames: the daemon sits next
    // to a login screen and the preview runs at a few fps.
    options.AddConfigEntry("session.intra_op.allow_spinning", "0");
    return options;
}

std::unique_ptr<Ort::Session> load_session(const std::filesystem::path& path) {
    auto options = make_session_options();
    // path.c_str() is wchar_t on Windows and char elsewhere, which is exactly
    // what ORTCHAR_T expects (full Unicode path support).
    return std::make_unique<Ort::Session>(shared_environment(), path.c_str(), options);
}

// One input/output tensor description for Session::Run.
struct Tensor {
    const char* name;
    float* data;
    std::size_t count;
    std::array<std::int64_t, 5> shape;
    std::size_t rank;
};

std::vector<Ort::Value> run_session(Ort::Session& session, std::span<const Tensor> inputs,
    std::span<const char* const> output_names) {
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto names = std::vector<const char*>{};
    auto values = std::vector<Ort::Value>{};
    names.reserve(inputs.size());
    values.reserve(inputs.size());
    for (const auto& input : inputs) {
        names.push_back(input.name);
        values.push_back(Ort::Value::CreateTensor<float>(
            memory, input.data, input.count, input.shape.data(), input.rank));
    }
    return session.Run(Ort::RunOptions{nullptr}, names.data(), values.data(), values.size(),
        output_names.data(), output_names.size());
}

std::span<const float> output_floats(const Ort::Value& value, std::size_t expected_count) {
    const auto count = value.GetTensorTypeAndShapeInfo().GetElementCount();
    if (count != expected_count) {
        throw std::runtime_error("unexpected model output size");
    }
    return {value.GetTensorData<float>(), count};
}

bool all_finite(std::span<const float> values) {
    return std::ranges::all_of(values, [](const float v) { return std::isfinite(v); });
}

// Interleaved RGB8 view of the caller's pixels. Colour order is RGB already:
// no channel swap (the SeetaFace path needed BGR).
struct RgbFrame {
    std::span<const std::uint8_t> pixels;
    std::vector<std::uint8_t> storage;
    int width = 0;
    int height = 0;
};

RgbFrame to_rgb(const ImageView image) {
    auto frame = RgbFrame{};
    frame.width = image.width;
    frame.height = image.height;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(image.bytes.data());
    const auto pixel_count = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    if (image.channels == 3) {
        frame.pixels = {bytes, pixel_count * 3};
        return frame;
    }
    frame.storage.resize(pixel_count * 3);
    for (auto i = std::size_t{}; i < pixel_count; ++i) {
        if (image.channels == 1) {
            frame.storage[i * 3] = frame.storage[i * 3 + 1] = frame.storage[i * 3 + 2] = bytes[i];
        } else {  // RGBA: drop alpha
            frame.storage[i * 3] = bytes[i * 4];
            frame.storage[i * 3 + 1] = bytes[i * 4 + 1];
            frame.storage[i * 3 + 2] = bytes[i * 4 + 2];
        }
    }
    frame.pixels = frame.storage;
    return frame;
}

FaceBox to_face_box(const lf::Detection& face) {
    const auto x = static_cast<int>(std::floor(face.x1));
    const auto y = static_cast<int>(std::floor(face.y1));
    return FaceBox{.x = x, .y = y,
        .width = static_cast<int>(std::ceil(face.x2)) - x,
        .height = static_cast<int>(std::ceil(face.y2)) - y};
}

}  // namespace

class LightFaceBackend::Impl {
public:
    explicit Impl(const LightFaceModelPaths& paths) {
        try {
            detector_ = load_session(paths.detector);
            recognizer_ = load_session(paths.recognizer);
        } catch (const std::exception& error) {
            su::log::process_logger().warn("lightface.model_load_failed",
                {su::log::field("cause", error.what())});
            detector_.reset();
            recognizer_.reset();
        } catch (...) {
            detector_.reset();
            recognizer_.reset();
        }
        // Liveness is optional: a missing or broken model only disables it.
        if (is_file(paths.fas_spatial) && is_file(paths.fas_temporal)) {
            try {
                spatial_ = load_session(paths.fas_spatial);
                temporal_ = load_session(paths.fas_temporal);
            } catch (const std::exception& error) {
                su::log::process_logger().warn("lightface.liveness_load_failed",
                    {su::log::field("cause", error.what())});
                spatial_.reset();
                temporal_.reset();
            } catch (...) {
                spatial_.reset();
                temporal_.reset();
            }
        }
    }

    std::expected<RecognitionResult, RecognizerError> run_pipeline(const ImageView image,
        const bool extract_feature, const bool liveness_enabled, const bool low_light_retry) const {
        std::lock_guard lock(mutex_);
        if (!available()) {
            return std::unexpected(RecognizerError::kModelUnavailable);
        }
        if (!valid_image(image)) {
            reset_liveness_locked();
            return std::unexpected(RecognizerError::kInvalidImage);
        }
        if (liveness_enabled && !liveness_available()) {
            return std::unexpected(RecognizerError::kModelUnavailable);
        }
        if (!liveness_enabled) {
            reset_liveness_locked();
        }
        try {
            return process(image, extract_feature, liveness_enabled, low_light_retry);
        } catch (const std::exception& error) {
            reset_liveness_locked();
            su::log::process_logger().warn("lightface.inference_failed",
                {su::log::field("cause", error.what())});
            return std::unexpected(RecognizerError::kModelUnavailable);
        }
    }

    std::expected<RecognitionResult, RecognizerError> extract(
        const ImageView image, const bool liveness_enabled, const bool low_light_retry) const {
        return run_pipeline(image, /*extract_feature=*/true, liveness_enabled, low_light_retry);
    }

    std::expected<RecognitionResult, RecognizerError> predict_liveness(
        const ImageView image, const bool liveness_enabled, const bool low_light_retry) const {
        return run_pipeline(image, /*extract_feature=*/false, liveness_enabled, low_light_retry);
    }

    bool available() const { return detector_ && recognizer_; }
    bool liveness_available() const { return spatial_ && temporal_; }

    void reset_liveness() {
        std::lock_guard lock(mutex_);
        reset_liveness_locked();
    }

private:
    struct FrameFeature {
        std::array<float, lf::kFasFeatDim> feature{};
        float quality = 0.0F;
    };

    std::expected<RecognitionResult, RecognizerError> process(const ImageView image,
        const bool extract_feature, const bool liveness_enabled, const bool low_light_retry) const {
        const auto frame = to_rgb(image);
        auto detections = detect(frame.pixels, frame.width, frame.height);
        auto enhanced_only = false;
        if (detections.empty() && low_light_retry) {
            detections = detect_enhanced(frame);
            enhanced_only = !detections.empty();
        }
        // Authentication must not silently choose one identity from a
        // multi-person frame, so anything but exactly one face yields no face.
        if (detections.size() != 1) {
            reset_liveness_locked();
            return RecognitionResult{};
        }
        const auto& face = detections.front();
        if (face.width() < lf::kMinFaceSidePx || face.height() < lf::kMinFaceSidePx) {
            reset_liveness_locked();
            return RecognitionResult{};
        }
        const auto box = to_face_box(face);
        if (enhanced_only && !face_bright_enough(frame, box)) {
            // Metering-only: the box lets the exposure loop brighten the
            // scene, but nothing downstream may see this near-black frame.
            reset_liveness_locked();
            return RecognitionResult{
                .has_face = false, .face_box = box, .feature = {}, .liveness_score = 0.0F};
        }

        auto result = RecognitionResult{.has_face = true, .face_box = box, .feature = {},
            .liveness_score = 1.0F};
        if (liveness_enabled) {
            result.liveness_score = score_liveness(frame, face);
        }
        if (extract_feature) {
            auto feature = embed(frame, face);
            if (!feature) {
                return std::unexpected(RecognizerError::kModelUnavailable);
            }
            result.feature = std::move(*feature);
        }
        return result;
    }

    std::vector<lf::Detection> detect(const std::span<const std::uint8_t> rgb, const int width,
        const int height) const {
        const auto letterbox = lf::letterbox_rgb(rgb, width, height);
        detector_input_.resize(3U * lf::kDetInputSize * lf::kDetInputSize);
        lf::pack_detector_input(letterbox, detector_input_);
        const auto inputs = std::array{Tensor{"rgb_01", detector_input_.data(), detector_input_.size(),
            {1, 3, lf::kDetInputSize, lf::kDetInputSize, 0}, 4}};
        constexpr auto kOutputs = std::array<const char*, 3>{"boxes_xyxy", "scores", "kpts5_xy"};
        const auto outputs = run_session(*detector_, inputs, kOutputs);
        const auto count = outputs.at(1).GetTensorTypeAndShapeInfo().GetElementCount();
        const auto boxes = output_floats(outputs.at(0), count * 4);
        const auto scores = output_floats(outputs.at(1), count);
        const auto keypoints = output_floats(outputs.at(2), count * 10);
        auto detections = lf::decode_and_nms(boxes, scores, keypoints);
        for (auto& detection : detections) {
            detection = lf::unletterbox_detection(
                detection, letterbox.scale, letterbox.pad_x, letterbox.pad_y, width, height);
        }
        return detections;
    }

    // Backlit faces can be too dark for the detector (#137). Only when the
    // plain pass finds nothing, retry on a gamma-brightened copy so the
    // exposure loop gets a face box to meter. The enhanced copy is used for
    // detection alone; landmarks, liveness and the embedding stay on the
    // untouched original so templates and liveness scores do not shift.
    std::vector<lf::Detection> detect_enhanced(const RgbFrame& frame) const {
        const auto bytes = std::as_bytes(frame.pixels);
        const auto centre = detail::measure_luma(bytes, frame.width, frame.height, 3,
            detail::metering_region(frame.width, frame.height, std::nullopt), detail::PixelOrder::rgb);
        const auto lut = detail::low_light_lut(centre).value_or(
            *detail::low_light_lut(detail::LumaStats{.mean = 60.0, .samples = 1}));
        // Reused every frame: the dark, face-less case is the steady state
        // while the exposure loop is still adjusting.
        enhance_scratch_.resize(frame.pixels.size());
        detail::apply_lut(frame.pixels, lut, enhance_scratch_);
        auto detections = detect(enhance_scratch_, frame.width, frame.height);
        log_low_light_detect(centre.mean, detections.size());
        return detections;
    }

    // Mean luma of the inner face region on the ORIGINAL (un-enhanced) image.
    bool face_bright_enough(const RgbFrame& frame, const FaceBox& box) const {
        const auto region = detail::metering_region(frame.width, frame.height,
            detail::PixelRegion{.x = box.x, .y = box.y, .width = box.width, .height = box.height});
        return detail::enhanced_face_is_usable(
            detail::measure_luma(std::as_bytes(frame.pixels), frame.width, frame.height, 3, region,
                detail::PixelOrder::rgb),
            lf::kFaceMeanLumaFloor);
    }

    // Streaming ST-FAS-Lite: the spatial encoder runs per frame (current and
    // previous texture crop plus the context crop); once eight consecutive
    // frames are buffered the temporal head turns them into a live probability.
    // The result is 0 while warming up and whenever the clip is too blurry.
    float score_liveness(const RgbFrame& frame, const lf::Detection& face) const {
        const auto crops = lf::crop_dual_scale_fas(frame.pixels, frame.width, frame.height, face);
        constexpr auto kPlane = 3U * lf::kFasCropSize * lf::kFasCropSize;
        texture_.resize(kPlane);
        context_.resize(kPlane);
        lf::pack_fas_input(crops.texture, texture_);
        lf::pack_fas_input(crops.context, context_);
        if (!texture_previous_valid_) {
            texture_previous_ = texture_;  // frame 0 of a clip is its own predecessor
            texture_previous_valid_ = true;
        }
        const auto shape = std::array<std::int64_t, 5>{1, 3, lf::kFasCropSize, lf::kFasCropSize, 0};
        const auto inputs = std::array{
            Tensor{"tex_curr", texture_.data(), kPlane, shape, 4},
            Tensor{"tex_prev", texture_previous_.data(), kPlane, shape, 4},
            Tensor{"ctx_curr", context_.data(), kPlane, shape, 4},
        };
        constexpr auto kSpatialOutputs = std::array<const char*, 2>{"spatial_feat", "frame_quality"};
        const auto spatial = run_session(*spatial_, inputs, kSpatialOutputs);
        const auto feature = output_floats(spatial.at(0), lf::kFasFeatDim);
        const auto quality = output_floats(spatial.at(1), 1);
        texture_previous_ = texture_;
        if (!all_finite(feature) || !all_finite(quality)) {
            reset_liveness_locked();
            return 0.0F;
        }
        auto entry = FrameFeature{.quality = quality[0]};
        std::ranges::copy(feature, entry.feature.begin());
        window_.push_back(entry);
        if (window_.size() > lf::kFasClipLen) {
            window_.pop_front();
        }
        if (window_.size() < lf::kFasClipLen) {
            return 0.0F;
        }

        sequence_.resize(lf::kFasClipLen * lf::kFasFeatDim);
        qualities_.resize(lf::kFasClipLen);
        for (auto t = std::size_t{}; t < lf::kFasClipLen; ++t) {
            std::ranges::copy(window_[t].feature, sequence_.begin()
                + static_cast<std::ptrdiff_t>(t * lf::kFasFeatDim));
            qualities_[t] = window_[t].quality;
        }
        const auto head_inputs = std::array{
            Tensor{"spatial_seq", sequence_.data(), sequence_.size(),
                {1, static_cast<std::int64_t>(lf::kFasClipLen), static_cast<std::int64_t>(lf::kFasFeatDim), 0, 0}, 3},
            Tensor{"quality_seq", qualities_.data(), qualities_.size(),
                {1, static_cast<std::int64_t>(lf::kFasClipLen), 1, 0, 0}, 3},
        };
        constexpr auto kHeadOutputs = std::array<const char*, 3>{"live_prob", "spoof_logits", "clarity_score"};
        const auto head = run_session(*temporal_, head_inputs, kHeadOutputs);
        const auto live = output_floats(head.at(0), 1)[0];
        const auto clarity = output_floats(head.at(2), 1)[0];
        const auto score = (!std::isfinite(live) || !std::isfinite(clarity)
                               || clarity < lf::kFasClarityThreshold)
            ? 0.0F
            : std::clamp(live, 0.0F, 1.0F);
        log_liveness(live, clarity, score);
        return score;
    }

    std::optional<std::vector<float>> embed(const RgbFrame& frame, const lf::Detection& face) const {
        const auto aligned = lf::align_face_112(frame.pixels, frame.width, frame.height, face.kpts);
        recognizer_input_.resize(3U * lf::kRecInputSize * lf::kRecInputSize);
        lf::pack_recognizer_input(aligned, recognizer_input_);
        const auto inputs = std::array{Tensor{"rgb_norm", recognizer_input_.data(),
            recognizer_input_.size(), {1, 3, lf::kRecInputSize, lf::kRecInputSize, 0}, 4}};
        constexpr auto kOutputs = std::array<const char*, 2>{"l2_emb", "feat_norm"};
        const auto outputs = run_session(*recognizer_, inputs, kOutputs);
        const auto embedding = output_floats(outputs.at(0), lf::kRecEmbeddingDim);
        const auto norm = output_floats(outputs.at(1), 1)[0];
        // EdgeFace's raw feature norm is ~2.6-3.0 on sharp faces; it is only a
        // sanity value here, not a quality gate.
        if (!all_finite(embedding) || !std::isfinite(norm) || norm <= 0.0F) {
            return std::nullopt;
        }
        auto feature = std::vector<float>(embedding.begin(), embedding.end());
        auto squared = 0.0;
        for (const auto value : feature) {
            squared += static_cast<double>(value) * value;
        }
        if (squared <= 0.0) {
            return std::nullopt;
        }
        const auto inverse = static_cast<float>(1.0 / std::sqrt(squared));
        for (auto& value : feature) {
            value *= inverse;
        }
        return feature;
    }

    void reset_liveness_locked() const {
        window_.clear();
        texture_previous_valid_ = false;
        next_liveness_log_ = {};
    }

    void log_liveness(const float live, const float clarity, const float score) const {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_liveness_log_) {
            return;
        }
        next_liveness_log_ = now + std::chrono::seconds{3};
        su::log::process_logger().debug("lightface.liveness", {
            su::log::field("live_prob", live), su::log::field("clarity", clarity),
            su::log::field("score", score), su::log::field("frames", window_.size())});
    }

    void log_low_light_detect(const double centre_mean, const std::size_t faces_found) const {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_low_light_log_) {
            return;
        }
        next_low_light_log_ = now + std::chrono::seconds{3};
        su::log::process_logger().debug("lightface.low_light_detect", {
            su::log::field("centre_mean", centre_mean),
            su::log::field("outcome", faces_found > 0 ? "found" : "none"),
            su::log::field("faces", faces_found)});
    }

    std::unique_ptr<Ort::Session> detector_;
    std::unique_ptr<Ort::Session> recognizer_;
    std::unique_ptr<Ort::Session> spatial_;
    std::unique_ptr<Ort::Session> temporal_;
    mutable std::mutex mutex_;
    // Everything below is guarded by mutex_.
    mutable std::vector<float> detector_input_;
    mutable std::vector<float> recognizer_input_;
    mutable std::vector<float> texture_;
    mutable std::vector<float> context_;
    mutable std::vector<float> texture_previous_;
    mutable std::vector<float> sequence_;
    mutable std::vector<float> qualities_;
    mutable std::vector<std::uint8_t> enhance_scratch_;
    mutable std::deque<FrameFeature> window_;
    mutable bool texture_previous_valid_ = false;
    mutable std::chrono::steady_clock::time_point next_liveness_log_{};
    mutable std::chrono::steady_clock::time_point next_low_light_log_{};
};

#else  // !SU_HAS_LIGHTFACE

class LightFaceBackend::Impl {
public:
    explicit Impl(const LightFaceModelPaths&) {}

    std::expected<RecognitionResult, RecognizerError> extract(ImageView, bool, bool) const {
        return std::unexpected(RecognizerError::kModelUnavailable);
    }
    std::expected<RecognitionResult, RecognizerError> predict_liveness(ImageView, bool, bool) const {
        return std::unexpected(RecognizerError::kModelUnavailable);
    }
    bool available() const { return false; }
    bool liveness_available() const { return false; }
    void reset_liveness() {}
};

#endif  // SU_HAS_LIGHTFACE

LightFaceBackend::LightFaceBackend(LightFaceModelPaths paths)
    : impl_(std::make_unique<Impl>(paths)) {}

LightFaceBackend::~LightFaceBackend() = default;

std::expected<RecognitionResult, RecognizerError> LightFaceBackend::extract(
    const ImageView image, const bool liveness_enabled, const bool low_light_retry) const {
    return impl_->extract(image, liveness_enabled, low_light_retry);
}

std::expected<RecognitionResult, RecognizerError> LightFaceBackend::predict_liveness(
    const ImageView image, const bool liveness_enabled, const bool low_light_retry) const {
    return impl_->predict_liveness(image, liveness_enabled, low_light_retry);
}

void LightFaceBackend::reset_liveness() {
    impl_->reset_liveness();
}

bool LightFaceBackend::available() const {
    return impl_->available();
}

bool LightFaceBackend::liveness_available() const {
    return impl_->liveness_available();
}

}  // namespace su::recognizer
