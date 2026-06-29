#include "app/preview_controller.h"

#if SU_HAS_SLINT

#include "recognizer/recognizer_service.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace su::app {

namespace {

// Detection runs on this fraction of the preview cadence. SeetaFace detection
// is expensive (~100-300ms); keeping it off the preview thread is what makes
// the preview smooth while the face box still updates regularly.
constexpr int kDetectionIntervalDivider = 4;

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

// The latest converted RGB frame, shared between the preview thread (producer)
// and the detection thread (consumer) under a mutex. Copied so each thread
// owns its data and neither races the next grab.
struct SharedRgb {
    int width = 0;
    int height = 0;
    std::vector<std::byte> bytes;
    bool fresh = false;  // set when a new frame is published; cleared on consume
};

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

        const auto interval = std::chrono::milliseconds(1000 / fps);
        const auto detect_interval = interval * kDetectionIntervalDivider;
        auto running = running_;

        // Detection thread: takes the freshest RGB snapshot, runs SeetaFace on
        // it, and publishes the overlay. Slow model work is fully off the
        // preview thread so it never stalls display.
        detection_thread_ = std::thread(
            [running, detect_interval, &recognizer, this]() {
                while (running->load()) {
                    SharedRgb snapshot;
                    {
                        std::lock_guard lock(rgb_mutex_);
                        if (shared_rgb_.fresh) {
                            snapshot = shared_rgb_;
                            shared_rgb_.fresh = false;
                        }
                    }
                    if (snapshot.width > 0 && !snapshot.bytes.empty()) {
                        const auto result = recognizer.extract_from_image(
                            su::recognizer::ImageView{
                                .width = snapshot.width,
                                .height = snapshot.height,
                                .channels = 3,
                                .bytes = std::span<const std::byte>(snapshot.bytes),
                            });
                        PreviewOverlay overlay;
                        if (result && result->has_face) {
                            overlay = overlay_from_result(*result);
                        } else {
                            overlay.status_text = result ? "no face" : "detect failed";
                        }
                        {
                            std::lock_guard lock(overlay_mutex_);
                            latest_overlay_ = std::move(overlay);
                        }
                    }
                    std::this_thread::sleep_for(detect_interval);
                }
            });

        // Preview thread: grab + convert + push image on every tick, carrying
        // the latest overlay (updated asynchronously by the detection thread).
        preview_thread_ = std::thread(
            [running, interval, callback = std::move(callback),
             &recognizer, this]() mutable {
                while (running->load()) {
                    const auto frame = recognizer.capture_preview_frame();
                    if (!frame) {
                        slint::invoke_from_event_loop(
                            [callback]() {
                                callback(slint::Image(),
                                         PreviewOverlay{.status_text = "capture failed"});
                            });
                        std::this_thread::sleep_for(interval);
                        continue;
                    }

                    auto image = preview_frame_to_image(*frame);

                    // Publish a copy for the detection thread.
                    {
                        std::lock_guard lock(rgb_mutex_);
                        shared_rgb_ = SharedRgb{
                            .width = frame->width,
                            .height = frame->height,
                            .bytes = frame->rgba_or_rgb,
                            .fresh = true,
                        };
                    }

                    // Snapshot the latest overlay to push alongside this frame.
                    PreviewOverlay overlay;
                    {
                        std::lock_guard lock(overlay_mutex_);
                        overlay = latest_overlay_;
                    }

                    slint::invoke_from_event_loop(
                        [callback, image = std::move(image),
                         overlay = std::move(overlay)]() mutable {
                            callback(std::move(image), std::move(overlay));
                        });

                    std::this_thread::sleep_for(interval);
                }
            });
    }

    void stop() {
        if (running_) {
            running_->store(false);
        }
        if (preview_thread_.joinable()) {
            preview_thread_.join();
        }
        if (detection_thread_.joinable()) {
            detection_thread_.join();
        }
    }

    [[nodiscard]] bool is_running() const {
        return running_ && running_->load()
            && (preview_thread_.joinable() || detection_thread_.joinable());
    }

    ~Impl() { stop(); }

private:
    std::thread preview_thread_;
    std::thread detection_thread_;
    std::shared_ptr<std::atomic<bool>> running_;
    std::mutex rgb_mutex_;
    SharedRgb shared_rgb_;
    std::mutex overlay_mutex_;
    PreviewOverlay latest_overlay_;
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