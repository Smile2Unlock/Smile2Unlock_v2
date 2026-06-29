#pragma once

#include "recognizer/recognizer_service.h"

#include <functional>
#include <optional>
#include <string>

#if SU_HAS_SLINT
#include <slint/slint.h>
#endif

namespace su::app {

// Overlay data derived from a preview frame: the detected face box, the
// liveness score, and a human-readable status line. Pushed alongside the
// preview image so the UI can draw the box and stats in one update.
struct PreviewOverlay {
    std::optional<su::recognizer::FaceBox> face_box;
    float liveness_score = 0.0F;
    std::string status_text;
};

#if SU_HAS_SLINT

// Owns a background capture thread that grabs camera frames, runs face
// detection on a throttled cadence, and marshals both the preview image and
// overlay onto the Slint event loop. All camera/IO side-effects live here;
// frame conversion and detection are delegated to RecognizerService.
class PreviewController {
public:
    using FrameCallback = std::function<void(slint::Image, PreviewOverlay)>;

    PreviewController() = default;
    ~PreviewController();
    PreviewController(const PreviewController&) = delete;
    PreviewController& operator=(const PreviewController&) = delete;
    PreviewController(PreviewController&&) = delete;
    PreviewController& operator=(PreviewController&&) = delete;

    // Begin capturing from the given camera at fps frames per second. Each
    // produced frame is handed to callback on the Slint event loop thread.
    void start(su::recognizer::RecognizerService& recognizer,
               int camera_index,
               int fps,
               FrameCallback callback);

    // Signal the capture thread to stop and join it.
    void stop();

    [[nodiscard]] bool is_running() const;

private:
    class Impl;
    // Raw pointer with out-of-line destruction: a unique_ptr member would
    // require Impl to be complete in every TU that sees this header, because
    // the deleter instantiates at the member declaration site. The destructor
    // is defined in the .cpp where Impl is complete.
    Impl* impl_ = nullptr;
};

#endif  // SU_HAS_SLINT

}  // namespace su::app
