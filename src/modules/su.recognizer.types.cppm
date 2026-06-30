export module su.recognizer.types;

import std;

export namespace su::recognizer {

enum class RecognizerError {
    kNoCamera,
    kCameraUnavailable,
    kModelUnavailable,
    kInvalidArgument,
    kInvalidImage,
    kImageLoadFailed,
    kNoFace,
};

struct CameraInfo {
    int index = 0;
    std::string name;
};

struct PreviewFrame {
    int width = 0;
    int height = 0;
    std::vector<std::byte> rgba_or_rgb;
};

struct FaceBox {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct RecognitionResult {
    bool has_face = false;
    std::optional<FaceBox> face_box;
    std::vector<float> feature;
    float liveness_score = 0.0F;
};

// Non-owning view over decoded image pixels. Channels must be 1, 3, or 4 to be
// accepted by the SeetaFace backend.
struct ImageView {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::span<const std::byte> bytes;
};

// A captured camera frame in its native V4L2 pixel format. The bytes are
// non-owning (they point at the mmap'ed V4L2 buffer) and only valid until the
// next grab_frame call. The v4l2_format field carries a V4L2_PIX_FMT_* value
// interpreted by v4l2_frame_to_rgb.
struct CapturedFrame {
    int width = 0;
    int height = 0;
    std::uint32_t v4l2_format = 0;
    std::span<const std::byte> bytes;
};

// Encode an embedding vector as a source string consumable by the Rust core.
// Inverse of the "embedding:" parser in the Rust profile store.
std::string embedding_sample_source(std::span<const float> feature);

} // namespace su::recognizer
