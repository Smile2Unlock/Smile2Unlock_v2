module;
#include <slint/slint.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <thread>

module su.app.preview;
import su.recognizer.service;

#if SU_HAS_SLINT

namespace su::app {

namespace {

slint::Image preview_frame_to_image(const su::recognizer::PreviewFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0
        || !std::in_range<std::uint32_t>(frame.width)
        || !std::in_range<std::uint32_t>(frame.height)) {
        return {};
    }
    const auto width = static_cast<std::size_t>(frame.width);
    const auto height = static_cast<std::size_t>(frame.height);
    if (width > std::numeric_limits<std::size_t>::max() / height
        || width * height > std::numeric_limits<std::size_t>::max() / 3
        || frame.rgba_or_rgb.size() < width * height * 3) {
        return {};
    }
    slint::SharedPixelBuffer<slint::Rgb8Pixel> buffer(
        static_cast<uint32_t>(frame.width),
        static_cast<uint32_t>(frame.height));
    const auto* src = reinterpret_cast<const unsigned char*>(frame.rgba_or_rgb.data());
    auto* dst = buffer.begin();
    const auto pixel_count = width * height;
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
    overlay.status_text = result.has_face ? "preview.face_liveness" : "preview.no_face";
    return overlay;
}

}  // namespace

// Single capture+detect thread. SeetaFace's FaceAntiSpoofing is a stateful
// video-stream model that must be fed on consecutive frames to reach a stable
// REAL/SPOOF verdict, so predict_liveness runs on EVERY preview frame (not a
// throttled subset). Feature extraction (the expensive, liveness-irrelevant
// step) is NOT done here; it only runs on explicit enroll/auth. This keeps the
// liveness score accurate while the preview stays on one thread with no
// cross-thread snapshot ownership.
class PreviewController::Impl {
public:
    void start(su::recognizer::RecognizerService& recognizer,
               int camera_index,
               int fps,
               bool liveness_enabled,
               FrameCallback callback) {
        stop();
        if (!callback) {
            std::println(stderr, "[preview] missing frame callback");
            return;
        }
        fps = std::clamp(fps, 1, 120);
        if (const auto opened = recognizer.open_camera(camera_index); !opened) {
            std::println(stderr, "[preview] open_camera failed");
            return;
        }
        if (liveness_enabled) {
            if (const auto reset = recognizer.reset_liveness(); !reset) {
                recognizer.close_camera();
                std::println(stderr, "[preview] liveness reset failed");
                return;
            }
        }
        recognizer_ = &recognizer;
        running_ = std::make_shared<std::atomic<bool>>(true);

        const auto interval = std::chrono::milliseconds(1000 / fps);
        auto running = running_;
        auto shared_callback = std::make_shared<FrameCallback>(std::move(callback));

        thread_ = std::thread(
            [running, interval, callback = std::move(shared_callback),
             &recognizer, liveness_enabled]() mutable {
                while (running->load(std::memory_order_relaxed)) {
                    auto frame = recognizer.capture_preview_frame();
                    if (!frame) {
                        slint::invoke_from_event_loop(
                            [running, callback]() {
                                if (running->load(std::memory_order_relaxed)) {
                                    (*callback)(slint::Image(),
                                                PreviewOverlay{.face_box = {}, .status_text = "preview.capture_failed"});
                                }
                            });
                        std::this_thread::sleep_for(interval);
                        continue;
                    }

                    auto image = preview_frame_to_image(*frame);

                    // Run detection + (optionally) liveness on this frame.
                    // predict_liveness feeds anti-spoofing every tick so the
                    // verdict stabilizes; when liveness is disabled in config
                    // the Predict call is skipped entirely and the score is
                    // pinned to 1.0 (passing).
                    auto result = recognizer.predict_liveness(su::recognizer::ImageView{
                        .width = frame->width,
                        .height = frame->height,
                        .channels = 3,
                        .bytes = std::span<const std::byte>(frame->rgba_or_rgb),
                    }, liveness_enabled);

                    PreviewOverlay overlay;
                    if (result && result->has_face) {
                        overlay = overlay_from_result(*result);
                        if (!liveness_enabled) {
                            overlay.status_text = "preview.liveness_disabled";
                        }
                    } else if (result && !result->has_face) {
                        overlay.status_text = "preview.no_face";
                    } else {
                        overlay.status_text = "preview.detect_failed";
                    }
                    overlay.source_width = frame->width;
                    overlay.source_height = frame->height;

                    slint::invoke_from_event_loop(
                        [running, callback, image = std::move(image),
                         overlay = std::move(overlay)]() mutable {
                            if (running->load(std::memory_order_relaxed)) {
                                (*callback)(std::move(image), std::move(overlay));
                            }
                        });

                    std::this_thread::sleep_for(interval);
                }
            });
    }

    void stop() {
        if (running_) {
            running_->store(false, std::memory_order_relaxed);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (recognizer_ != nullptr) {
            recognizer_->close_camera();
            recognizer_ = nullptr;
        }
        running_.reset();
    }

    [[nodiscard]] bool is_running() const {
        return running_ && running_->load(std::memory_order_relaxed) && thread_.joinable();
    }

    ~Impl() { stop(); }

private:
    std::thread thread_;
    std::shared_ptr<std::atomic<bool>> running_;
    su::recognizer::RecognizerService* recognizer_ = nullptr;
};

PreviewController::~PreviewController() {
    delete impl_;
}

void PreviewController::start(su::recognizer::RecognizerService& recognizer,
                              int camera_index,
                              int fps,
                              bool liveness_enabled,
                              FrameCallback callback) {
    if (!impl_) {
        impl_ = new Impl();
    }
    impl_->start(recognizer, camera_index, fps, liveness_enabled, std::move(callback));
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
