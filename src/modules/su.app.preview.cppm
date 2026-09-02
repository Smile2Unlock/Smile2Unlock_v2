module;
#include <slint/slint.h>

export module su.app.preview;

import std;
import su.recognizer.types;
import su.recognizer.service;

export namespace su::app {

// Overlay data derived from a preview frame.
struct PreviewOverlay {
    std::optional<su::recognizer::FaceBox> face_box;
    int source_width = 0;
    int source_height = 0;
    float liveness_score = 0.0F;
    std::string status_text;
};

#if SU_HAS_SLINT

class PreviewController {
public:
    using FrameCallback = std::move_only_function<void(slint::Image, PreviewOverlay)>;

    PreviewController() = default;
    ~PreviewController();
    PreviewController(const PreviewController&) = delete;
    PreviewController& operator=(const PreviewController&) = delete;
    PreviewController(PreviewController&&) = delete;
    PreviewController& operator=(PreviewController&&) = delete;

    void start(su::recognizer::RecognizerService& recognizer,
               int camera_index,
               int fps,
               bool liveness_enabled,
               FrameCallback callback);

    void stop();
    [[nodiscard]] bool is_running() const;

private:
    class Impl;
    Impl* impl_ = nullptr;
};

#endif // SU_HAS_SLINT

} // namespace su::app
