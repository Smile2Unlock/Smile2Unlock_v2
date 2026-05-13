module;

#include <expected>
#include <seeta/Common/CStruct.h>

#include "../protocol/messages.h"
#include "../seetaface.h"

export module su.facerecognizer.pipeline;

import std;
import su.facerecognizer.camera;

namespace su::facerecognizer {

struct ImageGuard {
    SeetaImageData& img;
    explicit ImageGuard(SeetaImageData& image) : img(image) {}
    ~ImageGuard() { delete[] img.data; }

    ImageGuard(const ImageGuard&) = delete;
    auto operator=(const ImageGuard&) -> ImageGuard& = delete;
    ImageGuard(ImageGuard&&) = delete;
    auto operator=(ImageGuard&&) -> ImageGuard& = delete;
};

}  // namespace su::facerecognizer

export namespace su::facerecognizer {

enum class PipelineError : std::uint8_t { kCaptureFailed, kNoFaceDetected, kSpoofRejected };

class RecognitionPipeline {
public:
    struct PreviewFrame {
        int width = 0;
        int height = 0;
        int channels = 0;
        std::vector<std::byte> image_bytes;
    };

    struct RecognitionEvent {
        bool matched = false;
        float similarity = 0.0F;
        std::vector<float> feature;
        std::optional<protocol::BoundingBox> face_box;
    };

    RecognitionPipeline(ICaptureDevice& capture_device, SeetaFaceEngine& engine)
        : capture_device_(capture_device), engine_(engine) {}

    void stop() {
        stop_requested_.store(true, std::memory_order_relaxed);
    }

    bool stopped() const {
        return stop_requested_.load(std::memory_order_relaxed);
    }

    std::expected<PreviewFrame, PipelineError> capture_preview_frame() {
        auto result = capture_device_.capture_frame();
        if (!result) {
            return std::unexpected(PipelineError::kCaptureFailed);
        }

        SeetaImageData image = *result;
        ImageGuard guard{image};

        return PreviewFrame{
            .width = image.width,
            .height = image.height,
            .channels = image.channels,
            .image_bytes = std::vector<std::byte>(
                reinterpret_cast<std::byte*>(image.data),
                reinterpret_cast<std::byte*>(image.data) +
                    static_cast<std::size_t>(image.width * image.height * image.channels)),
        };
    }

    std::expected<RecognitionEvent, PipelineError> recognize_once(
        bool liveness_detection, float liveness_threshold) {
        auto captured = capture_device_.capture_frame();
        if (!captured) {
            return std::unexpected(PipelineError::kCaptureFailed);
        }

        SeetaImageData image = *captured;
        ImageGuard guard{image};

        const auto face = engine_.detect(image);
        if (!face) {
            return std::unexpected(PipelineError::kNoFaceDetected);
        }

        if (liveness_detection) {
            auto spoof = engine_.anti_spoof(image, liveness_threshold);
            if (!spoof || !*spoof) {
                return std::unexpected(PipelineError::kSpoofRejected);
            }
        }

        auto feature = engine_.extract(image, engine_.mark(image, *face));
        return RecognitionEvent{
            .matched = !feature.empty(),
            .similarity = 0.0F,
            .feature = std::move(feature),
            .face_box = protocol::BoundingBox{
                .x = face->x,
                .y = face->y,
                .width = face->width,
                .height = face->height,
            },
        };
    }

private:
    ICaptureDevice& capture_device_;
    SeetaFaceEngine& engine_;
    std::atomic<bool> stop_requested_ = false;
};

}  // namespace su::facerecognizer
