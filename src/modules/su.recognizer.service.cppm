module;
#include "recognizer/exposure/exposure_control.h"

export module su.recognizer.service;

import std;
import su.recognizer.types;
import su.recognizer.backend;
import su.recognizer.camera;
import su.recognizer.image;

namespace su::recognizer::detail {

// Per-open-camera state of the backlight-aware exposure loop (issue #137).
struct ExposureSession {
    ExposureCapabilities caps;
    ExposureLoopState loop;
    std::optional<PixelRegion> applied_region;
    // Frame size of this camera session, learned from its first captured
    // frame; observations of any other size are ignored.
    int frame_width = 0;
    int frame_height = 0;
    // Consecutive transient write failures per knob (indexed by kind); a knob
    // is only dropped after kMaxTransientFailures in a row.
    std::array<int, kExposureKnobKindCount> transient_failures{};
    // Monotonic time when a face first went missing while controls/ROI were
    // active; cleared as soon as a face is seen again.
    std::optional<ExposureTime> face_missing_since;
    // Face mean luma measured right before manual_exposure was engaged,
    // used to detect cameras where manual mode disables AGC/dynamic fps.
    std::optional<double> pre_manual_mean;
    // Set once the loop wrote a control this session; a switch-off restore
    // that undoes it is a brightness step, so liveness must start over.
    bool wrote_control = false;
    // Switch-off restores that failed transiently, retried (throttled) on
    // later observations while the switch stays off.
    RestorePending restore_pending;
    ExposureTime next_restore_retry{};
    std::chrono::steady_clock::time_point last_region_change{};
    std::chrono::steady_clock::time_point next_log{};
};

}  // namespace su::recognizer::detail

export namespace su::recognizer {

class RecognizerService {
public:
    RecognizerService();
    ~RecognizerService();

    RecognizerService(const RecognizerService&) = delete;
    RecognizerService& operator=(const RecognizerService&) = delete;
    RecognizerService(RecognizerService&&) = delete;
    RecognizerService& operator=(RecognizerService&&) = delete;

    std::vector<CameraInfo> enumerate_cameras() const;
    std::expected<void, RecognizerError> open_camera(int camera_index);
    std::expected<PreviewFrame, RecognizerError> capture_preview_frame() const;
    std::expected<RecognitionResult, RecognizerError> extract_features(
        bool liveness_enabled = true) const;
    std::expected<float, RecognizerError> compare_features(
        std::span<const float> lhs,
        std::span<const float> rhs) const;
    void close_camera();

    std::expected<std::pair<PreviewFrame, RecognitionResult>, RecognizerError>
    capture_and_extract(bool liveness_enabled = true) const;

    std::expected<RecognitionResult, RecognizerError> extract_from_image(
        ImageView image, bool liveness_enabled = true) const;

    std::expected<RecognitionResult, RecognizerError> predict_liveness(
        ImageView image, bool liveness_enabled = true) const;
    std::expected<void, RecognizerError> reset_liveness() const;

    // Feeds one captured frame (and the face found in it, if any) to the
    // backlight-aware exposure loop. `camera_generation` is the value stamped
    // on the frame (PreviewFrame::camera_generation): frames of another camera
    // session, or of another size, are ignored. No-op without an open camera.
    // Returns true when a camera control was changed; the caller must then
    // reset the liveness window, which must not aggregate across a brightness
    // step. While the switch is off this only retries restores that failed
    // when it was turned off (and returns true when one succeeded).
    [[nodiscard]] bool observe_exposure(ImageView frame, std::optional<FaceBox> face,
        std::uint64_t camera_generation) const;

    // User switch for the whole backlight feature (issue #137): the exposure
    // loop and the low-light detection retry. Default off. Safe to call from
    // any thread while preview or authentication threads run. Turning it off
    // puts every camera control the loop changed back at once, and no further
    // control is written; any change of the switch restarts the loop from its
    // initial state.
    void set_backlight_compensation(bool enabled);
    [[nodiscard]] bool backlight_compensation() const;

    // True when the active face backend (LightFace by default) loaded its models.
    bool backend_available() const;
    // Short backend id ("lightface" / "seetaface"); empty while unavailable.
    std::string backend_name() const;
    // Feature vector dimension of the active backend (0 while unavailable).
    std::size_t embedding_dim() const;
    // Compatibility alias kept for callers that predate the backend layer.
    bool seetaface_available() const { return backend_available(); }

private:
    // Both return true when a camera control was actually put back, in which
    // case the liveness window has to start over.
    [[nodiscard]] bool restart_exposure_loop_unlocked() const;
    [[nodiscard]] bool in_exposure_session_unlocked(
        ImageView frame, std::uint64_t camera_generation) const;
    [[nodiscard]] bool retry_pending_restores_unlocked() const;

    std::expected<void, RecognizerError> ensure_backend() const;
    void close_camera_unlocked();
    std::expected<PreviewFrame, RecognizerError> grab_rgb_frame_locked() const;

    mutable std::mutex camera_mutex_;
    // Written without camera_mutex_; observe_exposure re-reads it under the
    // lock so a control is never written after the restore of a switch-off.
    std::atomic<bool> backlight_compensation_{false};
    std::optional<int> active_camera_;
    // Guarded by camera_mutex_.
    mutable detail::ExposureSession exposure_;
    // Bumped on every camera open and close; guarded by camera_mutex_.
    std::uint64_t camera_generation_ = 0;
    mutable std::unique_ptr<FaceBackend> backend_;
    mutable std::mutex backend_mutex_;
    mutable std::chrono::steady_clock::time_point next_backend_retry_{};
    std::unique_ptr<V4L2Camera> camera_;
};

// Encode an embedding vector as a source string consumable by the Rust core.
// Inverse of the "embedding:" parser in the Rust profile store.
std::string embedding_sample_source(std::span<const float> feature);

} // namespace su::recognizer
