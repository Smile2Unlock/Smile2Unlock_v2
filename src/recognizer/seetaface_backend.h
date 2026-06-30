#pragma once

#include "recognizer/recognizer_service.h"

#include <expected>
#include <filesystem>
#include <memory>

namespace su::recognizer {

struct SeetaFaceModelPaths {
    std::filesystem::path detector;
    std::filesystem::path landmarker;
    std::filesystem::path recognizer;
    std::filesystem::path anti_spoofing_first;
    std::filesystem::path anti_spoofing_second;
};

std::filesystem::path default_seetaface_model_dir();
std::expected<SeetaFaceModelPaths, RecognizerError> seetaface_model_paths(
    const std::filesystem::path& model_dir);

class SeetaFaceBackend {
public:
    explicit SeetaFaceBackend(SeetaFaceModelPaths paths);
    ~SeetaFaceBackend();

    SeetaFaceBackend(const SeetaFaceBackend&) = delete;
    SeetaFaceBackend& operator=(const SeetaFaceBackend&) = delete;
    SeetaFaceBackend(SeetaFaceBackend&&) noexcept;
    SeetaFaceBackend& operator=(SeetaFaceBackend&&) noexcept;

    // Detect + landmark + (optionally) feature extract + (optionally) anti-
    // spoofing Predict. liveness_enabled=false skips the Predict call and
    // returns liveness_score=1.0 so the frame still has a face box without the
    // liveness gate; callers gate this on config.liveness_detection.
    std::expected<RecognitionResult, RecognizerError> extract(
        ImageView image, bool liveness_enabled) const;
    // Detect + landmark + (optionally) anti-spoofing Predict, no feature
    // extraction. Intended to be called on every preview frame. When
    // liveness_enabled is false, Predict is skipped and liveness_score=1.0.
    std::expected<RecognitionResult, RecognizerError> predict_liveness(
        ImageView image, bool liveness_enabled) const;
    bool available() const;
    bool liveness_available() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace su::recognizer
