export module su.recognizer.backend;

import std;
import su.recognizer.types;

export namespace su::recognizer {

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

    std::expected<RecognitionResult, RecognizerError> extract(
        ImageView image, bool liveness_enabled) const;
    std::expected<RecognitionResult, RecognizerError> predict_liveness(
        ImageView image, bool liveness_enabled) const;
    void reset_liveness();
    bool available() const;
    bool liveness_available() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace su::recognizer
