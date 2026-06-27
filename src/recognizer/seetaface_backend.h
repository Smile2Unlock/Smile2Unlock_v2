#pragma once

#include "recognizer/recognizer_service.h"

#include <expected>
#include <filesystem>
#include <memory>
#include <span>

namespace su::recognizer {

struct ImageView {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::span<const std::byte> bytes;
};

struct SeetaFaceModelPaths {
    std::filesystem::path detector;
    std::filesystem::path landmarker;
    std::filesystem::path recognizer;
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
    bool available() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace su::recognizer
