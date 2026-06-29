#include "app/preview_controller.h"

#if SU_HAS_SLINT

#include "recognizer/recognizer_service.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace su::app {

namespace {

constexpr int kDetectionEveryNFrames = 4;

// Build a Slint image from an RGB PreviewFrame. Copies the pixels into a
// SharedPixelBuffer so the buffer outlives the mmap'ed source frame.
slint::Image preview_frame_to_image(const su::recognizer::PreviewFrame& frame) {
    slint::SharedPixelBuffer<slint::Rgb8Pixel> buffer(
        static_cast<uint32_t>(frame.width),
        static_cast<uint32_t>(frame.height));
    const auto* src = reinterpret_cast<const unsigned char*>(frame.rgba_or_rgb.data());
    auto* dst = buffer.begin();
    const auto pixel_count = static_cast<std::size_t>(frame.width) * frame.height;
    for (std::size_t i = 0; i < pixel_count; ++i) {
        dst[i] = slint::Rgb8Pixel{
            .r = src[i * 3 + 0],
            .g = src[i * 3 + 1],
            .b = src[i * 3 + 2],
        };
    }
    return slint::Image(std::move(buffer));
}

PreviewOverlay overlay_from_result(const su::recognizer::RecognitionResult& result) {
    PreviewOverlay overlay;
    overlay.face_box = result.face_box;
    overlay.liveness_score = result.liveness_score;
    overlay.status_text = result.has_face
        ? "face: liveness " + std::to_string(result.liveness_score)
        : "no face";
    return overlay;
}

}  // namespace

class PreviewController::Impl {
public:
    void start(su::recognizer::RecognizerService& recognizer,
               int camera_index,
               int fps,
               FrameCallback callback) {
        stop();
        if (fps <= 0) {
            fps = 15;
        }
        running_ = std::make_shared<std::atomic<bool>>(true);
        auto running = running_;
        const auto interval = std::chrono::milliseconds(1000 / fps);

        thread_ = std::thread(
            [running, interval, camera_index, callback = std::move(callback),
             &recognizer]() mutable {
                std::optional<su::recognizer::FaceBox> last_box;
                float last_liveness = 0.0F;
                std::string last_status = "starting";
                int frame_index = 0;

                while (running->load()) {
                    // capture_and_extract grabs exactly one frame and runs
                    // detection on it, so the preview image and the face box
                    // always correspond to the same capture. Detection runs on
                    // every grab but is throttled by the caller's fps; the Nth
                    // frame reuse below only avoids re-detecting when we have a
                    // cached frame without a fresh grab.
                    const bool detect = (frame_index % kDetectionEveryNFrames) == 0;

                    if (detect) {
                        const auto captured = recognizer.capture_and_extract();
                        if (!captured) {
                            last_status = "capture failed";
                            push_frame(callback, slint::Image(),
                                       PreviewOverlay{.status_text = last_status});
                            std::this_thread::sleep_for(interval);
                            ++frame_index;
                            continue;
                        }
                        auto& [frame, result] = *captured;
                        auto image = preview_frame_to_image(frame);
                        if (result.has_face) {
                            auto overlay = overlay_from_result(result);
                            last_box = overlay.face_box;
                            last_liveness = overlay.liveness_score;
                            last_status = overlay.status_text;
                            push_frame(callback, std::move(image), std::move(overlay));
                        } else {
                            last_box.reset();
                            last_liveness = 0.0F;
                            last_status = "no face";
                            push_frame(callback, std::move(image),
                                       PreviewOverlay{.status_text = last_status});
                        }
                    } else {
                        // Skip detection this frame: grab only for the preview
                        // image, reuse the last overlay.
                        const auto frame = recognizer.capture_preview_frame();
                        if (!frame) {
                            last_status = "capture failed";
                            push_frame(callback, slint::Image(),
                                       PreviewOverlay{.status_text = last_status});
                            std::this_thread::sleep_for(interval);
                            ++frame_index;
                            continue;
                        }
                        auto image = preview_frame_to_image(*frame);
                        push_frame(callback, std::move(image),
                                   PreviewOverlay{
                                       .face_box = last_box,
                                       .liveness_score = last_liveness,
                                       .status_text = last_status,
                                   });
                    }

                    ++frame_index;
                    std::this_thread::sleep_for(interval);
                }
            });
    }

    void stop() {
        if (running_) {
            running_->store(false);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] bool is_running() const {
        return running_ && running_->load() && thread_.joinable();
    }

    ~Impl() { stop(); }

private:
    static void push_frame(const FrameCallback& callback, slint::Image image, PreviewOverlay overlay) {
        slint::invoke_from_event_loop(
            [callback, image = std::move(image), overlay = std::move(overlay)]() mutable {
                callback(std::move(image), std::move(overlay));
            });
    }

    std::thread thread_;
    std::shared_ptr<std::atomic<bool>> running_;
};

PreviewController::~PreviewController() {
    delete impl_;
}

void PreviewController::start(su::recognizer::RecognizerService& recognizer,
                              int camera_index,
                              int fps,
                              FrameCallback callback) {
    if (!impl_) {
        impl_ = new Impl();
    }
    impl_->start(recognizer, camera_index, fps, std::move(callback));
}

void PreviewController::stop() {
    if (impl_) {
        impl_->stop();
    }
}

bool PreviewController::is_running() const {
    return impl_ != nullptr && impl_->is_running();
}

}  // namespace su::app

#endif  // SU_HAS_SLINT
