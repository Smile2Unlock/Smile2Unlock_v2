export module su.recognizer.service;

import std;
import su.recognizer.types;
import su.recognizer.backend;
import su.recognizer.camera;
import su.recognizer.image;

export namespace su::recognizer {

class RecognizerService {
public:
    RecognizerService();
    ~RecognizerService();

    RecognizerService(const RecognizerService&) = delete;
    RecognizerService& operator=(const RecognizerService&) = delete;
    RecognizerService(RecognizerService&&) = delete;
    RecognizerService& operator=(RecognizerService&&) = delete;

    std::vector<CameraInfo> enumerate_cameras() const;
    std::expected<void, RecognizerError> open_camera(int camera_index);
    std::expected<PreviewFrame, RecognizerError> capture_preview_frame() const;
    std::expected<RecognitionResult, RecognizerError> extract_features(
        bool liveness_enabled = true) const;
    std::expected<float, RecognizerError> compare_features(
        std::span<const float> lhs,
        std::span<const float> rhs) const;
    void close_camera();

    std::expected<std::pair<PreviewFrame, RecognitionResult>, RecognizerError>
    capture_and_extract(bool liveness_enabled = true) const;

    std::expected<RecognitionResult, RecognizerError> extract_from_image(
        ImageView image, bool liveness_enabled = true) const;

    std::expected<RecognitionResult, RecognizerError> predict_liveness(
        ImageView image, bool liveness_enabled = true) const;
    std::expected<void, RecognizerError> reset_liveness() const;

    bool seetaface_available() const;

private:
    std::expected<void, RecognizerError> ensure_seetaface_backend() const;
    void close_camera_unlocked();

    mutable std::mutex camera_mutex_;
    std::optional<int> active_camera_;
#if SU_HAS_SEETAFACE
    mutable std::unique_ptr<SeetaFaceBackend> seetaface_backend_;
    mutable std::unique_ptr<std::once_flag> seetaface_init_flag_;
#endif
    std::unique_ptr<V4L2Camera> camera_;
};

// Encode an embedding vector as a source string consumable by the Rust core.
// Inverse of the "embedding:" parser in the Rust profile store.
std::string embedding_sample_source(std::span<const float> feature);

} // namespace su::recognizer
