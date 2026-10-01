module;
#include <slint/slint.h>
#include "recognizer/latest_frame.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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

// Capture and inference have independent cadences. The inference mailbox
// retains one immutable frame, so slow anti-spoofing cannot block preview or
// accumulate a backlog. Each consumed frame is evaluated exactly once.
class PreviewController::Impl {
    struct State {
        su::recognizer::detail::LatestFrameQueue<su::recognizer::PreviewFrame> frames;
        std::mutex mutex;
        PreviewOverlay overlay;
        std::chrono::steady_clock::time_point overlay_frame_time{};
        std::optional<std::pair<slint::Image, PreviewOverlay>> pending_ui;
        bool ui_scheduled = false;
    };

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
        state_ = std::make_shared<State>();

        const auto interval = std::chrono::milliseconds(1000 / fps);
        auto running = running_;
        auto state = state_;
        auto shared_callback = std::make_shared<FrameCallback>(std::move(callback));

        inference_thread_ = std::thread([running, state, &recognizer, liveness_enabled] {
            auto consumed = std::uint64_t{};
            auto next_log = std::chrono::steady_clock::now();
            while (running->load(std::memory_order_relaxed)) {
                const auto frame = state->frames.wait_next(consumed);
                if (!frame) {
                    break;
                }
                const auto frame_time = std::chrono::steady_clock::now();
                const auto result = recognizer.predict_liveness(su::recognizer::ImageView{
                    .width = frame->width,
                    .height = frame->height,
                    .channels = 3,
                    .bytes = std::span<const std::byte>(frame->rgba_or_rgb),
                }, liveness_enabled);
                const auto finished = std::chrono::steady_clock::now();
                if (finished >= next_log) {
                    std::println(stderr, "[preview] inference_ms={}",
                        std::chrono::duration_cast<std::chrono::milliseconds>(finished - frame_time).count());
                    next_log = finished + std::chrono::seconds{5};
                }
                auto overlay = PreviewOverlay{};
                if (result && result->has_face) {
                    overlay = overlay_from_result(*result);
                    if (!liveness_enabled) {
                        overlay.status_text = "preview.liveness_disabled";
                    }
                } else {
                    overlay.status_text = result ? "preview.no_face" : "preview.detect_failed";
                }
                overlay.source_width = frame->width;
                overlay.source_height = frame->height;
                std::lock_guard lock(state->mutex);
                state->overlay = std::move(overlay);
                state->overlay_frame_time = frame_time;
            }
        });

        thread_ = std::thread(
            [running, state, interval, callback = std::move(shared_callback),
             &recognizer]() mutable {
                auto measured_since = std::chrono::steady_clock::now();
                auto captured_count = 0;
                const auto publish_ui = [&](slint::Image image, PreviewOverlay overlay) {
                    {
                        std::lock_guard lock(state->mutex);
                        state->pending_ui.emplace(std::move(image), std::move(overlay));
                        if (state->ui_scheduled) {
                            return;
                        }
                        state->ui_scheduled = true;
                    }
                    slint::invoke_from_event_loop([running, state, callback]() mutable {
                        std::optional<std::pair<slint::Image, PreviewOverlay>> latest;
                        {
                            std::lock_guard lock(state->mutex);
                            latest.swap(state->pending_ui);
                            state->ui_scheduled = false;
                        }
                        if (latest && running->load(std::memory_order_relaxed)) {
                            (*callback)(std::move(latest->first), std::move(latest->second));
                        }
                    });
                };
                while (running->load(std::memory_order_relaxed)) {
                    const auto tick = std::chrono::steady_clock::now();
                    auto frame = recognizer.capture_preview_frame();
                    if (!frame) {
                        publish_ui(slint::Image(), PreviewOverlay{
                            .face_box = {}, .status_text = "preview.capture_failed"});
                        std::this_thread::sleep_until(tick + interval);
                        continue;
                    }

                    const auto shared_frame = std::make_shared<const su::recognizer::PreviewFrame>(
                        std::move(*frame));
                    state->frames.publish(shared_frame);
                    auto overlay = PreviewOverlay{};
                    {
                        std::lock_guard lock(state->mutex);
                        // Overlay is informational only. Expire old geometry
                        // and scores instead of displaying a frozen verdict.
                        if (state->overlay.source_width == shared_frame->width
                            && state->overlay.source_height == shared_frame->height) {
                            overlay = state->overlay;
                            if (std::chrono::steady_clock::now() - state->overlay_frame_time
                                > std::chrono::milliseconds{500}) {
                                overlay.face_box.reset();
                            }
                            if (std::chrono::steady_clock::now() - state->overlay_frame_time
                                > std::chrono::seconds{2}) {
                                overlay.liveness_score = 0.0F;
                                overlay.status_text = "preview.starting";
                            }
                        } else {
                            overlay.status_text = "preview.starting";
                        }
                    }
                    overlay.source_width = shared_frame->width;
                    overlay.source_height = shared_frame->height;
                    publish_ui(preview_frame_to_image(*shared_frame), std::move(overlay));
                    ++captured_count;
                    const auto measured_until = std::chrono::steady_clock::now();
                    const auto seconds = std::chrono::duration<double>(measured_until - measured_since).count();
                    if (seconds >= 5.0) {
                        std::println(stderr, "[preview] capture_fps={:.1f} size={}x{}",
                            captured_count / seconds, shared_frame->width, shared_frame->height);
                        measured_since = measured_until;
                        captured_count = 0;
                    }

                    // Work time counts toward the requested frame period.
                    std::this_thread::sleep_until(tick + interval);
                }
            });
    }

    void stop() {
        if (running_) {
            running_->store(false, std::memory_order_relaxed);
        }
        if (state_) {
            state_->frames.close();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (inference_thread_.joinable()) {
            inference_thread_.join();
        }
        if (recognizer_ != nullptr) {
            recognizer_->close_camera();
            recognizer_ = nullptr;
        }
        running_.reset();
        state_.reset();
    }

    [[nodiscard]] bool is_running() const {
        return running_ && running_->load(std::memory_order_relaxed) && thread_.joinable();
    }

    ~Impl() { stop(); }

private:
    std::thread thread_;
    std::thread inference_thread_;
    std::shared_ptr<State> state_;
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
