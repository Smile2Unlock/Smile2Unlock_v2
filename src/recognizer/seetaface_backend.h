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

    std::expected<RecognitionResult, RecognizerError> extract(ImageView image) const;
    // Detect the face and run the anti-spoofing Predict only (no feature
    // extraction). Cheaper than extract and intended to be called on every
    // preview frame so FaceAntiSpoofing sees a continuous video stream and can
    // reach a stable REAL/SPOOF verdict. Returns has_face + face_box +
    // liveness_score (no feature). Thread-safe via an internal mutex.
    std::expected<RecognitionResult, RecognizerError> predict_liveness(ImageView image) const;
    bool available() const;
    bool liveness_available() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace su::recognizer
