#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace su::facerecognizer::protocol {

enum class CommandType : std::uint16_t {
    kUnknown = 0,
    kStartCameraPreview = 1,
    kStopCameraPreview = 2,
    kCaptureFrame = 3,
    kStartRecognition = 4,
    kStopRecognition = 5,
    kCompareFeatures = 6,
};

enum class MessageType : std::uint16_t {
    kError = 0,
    kPreviewFrame = 1,
    kCaptureResult = 2,
    kRecognitionResult = 3,
    kCompareResult = 4,
};

struct BoundingBox {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct CommandMessage {
    CommandType command = CommandType::kUnknown;
    int camera_index = 0;
    int preview_fps = 15;
    bool extract_feature = false;
    bool liveness_detection = false;
    float face_threshold = 0.62f;
    float liveness_threshold = 0.80f;
    std::string username_hint;
    std::vector<float> feature_a;
    std::vector<float> feature_b;
};

struct PreviewFrameMessage {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<std::byte> jpeg_bytes;
};

struct CaptureResultMessage {
    bool ok = false;
    std::string error_message;
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<std::byte> image_bytes;
    std::vector<float> feature;
};

struct RecognitionResultMessage {
    bool matched = false;
    std::string username;
    float similarity = 0.0f;
    std::optional<BoundingBox> face_box;
    std::vector<float> feature;
};

struct CompareResultMessage {
    bool ok = false;
    float similarity = 0.0f;
    std::string error_message;
};

struct OutboundMessage {
    MessageType type = MessageType::kError;
    std::string error_message;
    std::optional<PreviewFrameMessage> preview_frame;
    std::optional<CaptureResultMessage> capture_result;
    std::optional<RecognitionResultMessage> recognition_result;
    std::optional<CompareResultMessage> compare_result;
};

}  // namespace su::facerecognizer::protocol
