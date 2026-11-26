export module su.recognizer.backend;

import std;
import su.recognizer.types;

export namespace su::recognizer {

// Face recognition backends. LightFace (ONNX Runtime) is the product default;
// SeetaFace stays available behind the same interface when the build enables
// `--with_seetaface=y` (select at runtime with SU_RECOGNIZER_BACKEND).
enum class BackendKind {
    kLightFace,
    kSeetaFace,
};

// Compatibility layer: everything above the backend (service, daemon, GUI,
// Windows agent) only talks to this interface. Embeddings of different
// backends are NOT interchangeable; the profile store locks the dimension at
// the first enrollment (LightFace 512-d, SeetaFace 1024-d).
class FaceBackend {
public:
    FaceBackend() = default;
    virtual ~FaceBackend() = default;

    FaceBackend(const FaceBackend&) = delete;
    FaceBackend& operator=(const FaceBackend&) = delete;

    [[nodiscard]] virtual BackendKind kind() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual std::size_t embedding_dim() const noexcept = 0;

    // `low_light_retry` enables the brightened second detection pass for dark
    // frames; off, the first detector result is final.
    [[nodiscard]] virtual std::expected<RecognitionResult, RecognizerError> extract(
        ImageView image, bool liveness_enabled, bool low_light_retry = true) const = 0;
    [[nodiscard]] virtual std::expected<RecognitionResult, RecognizerError> predict_liveness(
        ImageView image, bool liveness_enabled, bool low_light_retry = true) const = 0;
    virtual void reset_liveness() = 0;
    [[nodiscard]] virtual bool available() const = 0;
    [[nodiscard]] virtual bool liveness_available() const = 0;
};

// --- LightFace (ONNX Runtime) -------------------------------------------------

struct LightFaceModelPaths {
    std::filesystem::path detector;
    std::filesystem::path recognizer;
    std::filesystem::path fas_spatial;
    std::filesystem::path fas_temporal;
};

std::filesystem::path default_lightface_model_dir();
// The detector and recognizer are required; the two liveness models are
// optional (a missing one only disables liveness, like the SeetaFace path).
std::expected<LightFaceModelPaths, RecognizerError> lightface_model_paths(
    const std::filesystem::path& model_dir);

class LightFaceBackend final : public FaceBackend {
public:
    explicit LightFaceBackend(LightFaceModelPaths paths);
    ~LightFaceBackend() override;

    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::kLightFace; }
    [[nodiscard]] std::string_view name() const noexcept override { return "lightface"; }
    [[nodiscard]] std::size_t embedding_dim() const noexcept override { return 512; }

    [[nodiscard]] std::expected<RecognitionResult, RecognizerError> extract(
        ImageView image, bool liveness_enabled, bool low_light_retry = true) const override;
    [[nodiscard]] std::expected<RecognitionResult, RecognizerError> predict_liveness(
        ImageView image, bool liveness_enabled, bool low_light_retry = true) const override;
    void reset_liveness() override;
    [[nodiscard]] bool available() const override;
    [[nodiscard]] bool liveness_available() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// --- SeetaFace (legacy, optional) ---------------------------------------------

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

class SeetaFaceBackend final : public FaceBackend {
public:
    explicit SeetaFaceBackend(SeetaFaceModelPaths paths);
    ~SeetaFaceBackend() override;

    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::kSeetaFace; }
    [[nodiscard]] std::string_view name() const noexcept override { return "seetaface"; }
    [[nodiscard]] std::size_t embedding_dim() const noexcept override { return 1024; }

    [[nodiscard]] std::expected<RecognitionResult, RecognizerError> extract(
        ImageView image, bool liveness_enabled, bool low_light_retry = true) const override;
    [[nodiscard]] std::expected<RecognitionResult, RecognizerError> predict_liveness(
        ImageView image, bool liveness_enabled, bool low_light_retry = true) const override;
    void reset_liveness() override;
    [[nodiscard]] bool available() const override;
    [[nodiscard]] bool liveness_available() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// --- Selection ------------------------------------------------------------------

// SU_RECOGNIZER_BACKEND=lightface|seetaface overrides the build default
// (LightFace when compiled in, else SeetaFace).
BackendKind active_default_backend_kind();
// Model directory of the default backend.
std::filesystem::path default_model_dir();
std::expected<std::unique_ptr<FaceBackend>, RecognizerError> create_backend(BackendKind kind);
std::expected<std::unique_ptr<FaceBackend>, RecognizerError> create_default_backend();

} // namespace su::recognizer
