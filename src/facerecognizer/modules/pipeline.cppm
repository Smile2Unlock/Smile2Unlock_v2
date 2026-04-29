module;

#include <seeta/Common/CStruct.h>
#include <tbox/coroutine/channel.h>

#include "../protocol/messages.h"
#include "../seetaface.h"

export module su.facerecognizer.pipeline;

import std;
import su.facerecognizer.camera;

export namespace su::facerecognizer {

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
        float similarity = 0.0f;
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

    std::optional<PreviewFrame> capture_preview_frame() {
        SeetaImageData image{};
        if (!capture_device_.capture_frame(image) || image.data == nullptr) {
            return std::nullopt;
        }

        PreviewFrame frame{
            .width = image.width,
            .height = image.height,
            .channels = image.channels,
            .image_bytes = std::vector<std::byte>(
                reinterpret_cast<std::byte*>(image.data),
                reinterpret_cast<std::byte*>(image.data) +
                    static_cast<std::size_t>(image.width * image.height * image.channels)),
        };
        delete[] image.data;
        return frame;
    }

    std::optional<RecognitionEvent> recognize_once(bool liveness_detection, float liveness_threshold) {
        SeetaImageData image{};
        if (!capture_device_.capture_frame(image) || image.data == nullptr) {
            return std::nullopt;
        }

        const SeetaRect face = engine_.detect(image);
        if (face.width <= 0 || face.height <= 0) {
            delete[] image.data;
            return std::nullopt;
        }

        if (liveness_detection && !engine_.anti_spoof(image, liveness_threshold)) {
            delete[] image.data;
            return std::nullopt;
        }

        auto feature = engine_.image_to_features(image);
        RecognitionEvent event{
            .matched = !feature.empty(),
            .similarity = 0.0f,
            .feature = std::move(feature),
            .face_box = protocol::BoundingBox{
                .x = face.x,
                .y = face.y,
                .width = face.width,
                .height = face.height,
            },
        };
        delete[] image.data;
        return event;
    }

private:
    ICaptureDevice& capture_device_;
    SeetaFaceEngine& engine_;
    std::atomic<bool> stop_requested_ = false;
};

}  // namespace su::facerecognizer
