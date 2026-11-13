module;
#include "common/log/logger.h"
#include "recognizer/exposure/exposure_control.h"

module su.recognizer.service;
import std;
import su.recognizer.camera;
import su.recognizer.types;

namespace su::recognizer {

namespace {

using Clock = std::chrono::steady_clock;
using detail::ExposureSession;

// Moving the camera's metering window too often makes its auto exposure
// chase it; only follow the face when it moved by a tenth of the frame.
constexpr int kRegionMovePercent = 10;
constexpr auto kRegionMinInterval = std::chrono::milliseconds{500};
constexpr auto kLogInterval = std::chrono::seconds{2};
// A knob that keeps failing with transient errors (busy, I/O) is given up on
// after this many writes in a row; a rejection drops it at once.
constexpr int kMaxTransientFailures = 3;

bool region_moved_notably(const detail::PixelRegion& from, const detail::PixelRegion& to,
    const ImageView& frame) {
    const auto limit_x = frame.width * kRegionMovePercent / 100;
    const auto limit_y = frame.height * kRegionMovePercent / 100;
    return std::abs(from.x - to.x) > limit_x || std::abs(from.y - to.y) > limit_y
        || std::abs(from.width - to.width) > limit_x
        || std::abs(from.height - to.height) > limit_y;
}

void update_metering_region(V4L2Camera& camera, ExposureSession& session,
    const detail::PixelRegion& region, const ImageView& frame, const Clock::time_point now) {
    if (!session.caps.metering_region) {
        return;
    }
    const auto first = !session.applied_region;
    if (!first && (now - session.last_region_change < kRegionMinInterval
                   || !region_moved_notably(*session.applied_region, region, frame))) {
        return;
    }
    session.last_region_change = now;
    const auto applied = camera.set_metering_region(region);
    if (applied) {
        session.applied_region = region;
        return;
    }
    if (applied.error() == detail::ExposureWriteError::transient) {
        return;  // try again at the next move; the region stays enabled
    }
    session.caps.metering_region = false;
    su::log::process_logger().warn("camera.exposure_region_failed", {
        su::log::field("error", detail::to_string(applied.error()))});
}

// The level now in effect, or why the camera refused.
std::expected<int, detail::ExposureWriteError> write_change(
    V4L2Camera& camera, const detail::ExposureChange& change) {
    if (change.action == detail::ExposureAction::set) {
        return camera.set_exposure_level(change.kind, change.level);
    }
    return camera.reset_exposure_knob(change.kind).transform([&] { return change.level; });
}

// Gives up on a knob for this session so the loop skips or escalates past it.
// The camera keeps its own record of what it touched, so close still restores
// it even when this best-effort reset fails (the failure is logged with errno
// by the camera layer and reported here).
void drop_knob(V4L2Camera& camera, ExposureSession& session,
    const detail::ExposureChange& change, const detail::ExposureWriteError error) {
    session.caps.knobs = session.caps.knobs.without(change.kind);
    const auto reset = camera.reset_exposure_knob(change.kind);
    su::log::process_logger().warn("camera.exposure_knob_failed", {
        su::log::field("knob", detail::to_string(change.kind)),
        su::log::field("level", change.level),
        su::log::field("error", detail::to_string(error)),
        su::log::field("reset", reset ? "ok" : detail::to_string(reset.error()))});
}

void handle_write_failure(V4L2Camera& camera, ExposureSession& session,
    const detail::ExposureChange& change, const detail::ExposureWriteError error) {
    auto& failures = session.transient_failures[std::to_underlying(change.kind)];
    if (error == detail::ExposureWriteError::transient && ++failures < kMaxTransientFailures) {
        return;  // keep the loop state: the same change is retried next observation
    }
    failures = 0;
    drop_knob(camera, session, change, error);
}

// Applies the decision and keeps the loop state; true when the camera changed.
bool apply_step(V4L2Camera& camera, ExposureSession& session, const detail::ExposureStep& step) {
    if (!step.change) {
        session.loop = step.next;
        return false;
    }
    const auto& change = *step.change;
    const auto outcome = write_change(camera, change);
    if (!outcome) {
        handle_write_failure(camera, session, change, outcome.error());
        return false;
    }
    session.transient_failures[std::to_underlying(change.kind)] = 0;
    session.loop = detail::with_applied_level(step.next, change.kind, *outcome);
    return true;
}

void log_decision(ExposureSession& session, const detail::LumaStats& stats,
    const Clock::time_point now) {
    if (now < session.next_log) {
        return;
    }
    session.next_log = now + kLogInterval;
    const auto& chain = session.caps.knobs;
    const auto active = chain.empty()
        ? std::nullopt
        : std::optional(chain[detail::active_position(chain, session.loop)]);
    su::log::process_logger().debug("camera.exposure_observe", {
        su::log::field("mean", stats.mean), su::log::field("clipped", stats.clipped_fraction),
        su::log::field("level", active ? session.loop.levels[std::to_underlying(active->kind)] : 0),
        su::log::field("knob", active ? detail::to_string(active->kind) : std::string_view{"none"})});
}

}  // namespace

void RecognizerService::set_backlight_compensation(const bool enabled) {
    if (backlight_compensation_.exchange(enabled) == enabled) {
        return;
    }
    std::lock_guard lock(camera_mutex_);
    restart_exposure_loop_unlocked();
}

bool RecognizerService::backlight_compensation() const {
    return backlight_compensation_.load();
}

// Puts back whatever the loop changed in this session (a no-op when nothing
// was) and starts the loop over. Nothing is written while the switch is off,
// so on re-enable the loop begins at the camera's own state. The generation
// is untouched: the session is the same, only the loop restarts.
void RecognizerService::restart_exposure_loop_unlocked() const {
    if (camera_ && active_camera_) {
        for (const auto kind : detail::kRestoreOrder) {
            (void)camera_->reset_exposure_knob(kind);
        }
        (void)camera_->reset_metering_region();
    }
    exposure_.loop = detail::make_exposure_state(exposure_.caps.knobs);
    exposure_.applied_region.reset();
    exposure_.transient_failures = {};
}

bool RecognizerService::observe_exposure(const ImageView frame, const std::optional<FaceBox> face,
    const std::uint64_t camera_generation) const {
    if (!backlight_compensation_.load()) {
        return false;
    }
    // Region and luma depend only on the frame, so they are computed before
    // taking the camera lock: capture and control writes are not held up by
    // pixel scanning.
    const auto face_region = face
        ? std::optional(detail::PixelRegion{face->x, face->y, face->width, face->height})
        : std::nullopt;
    const auto region = detail::metering_region(frame.width, frame.height, face_region);
    const auto stats = detail::measure_luma(
        frame.bytes, frame.width, frame.height, frame.channels, region);

    std::lock_guard lock(camera_mutex_);
    if (!backlight_compensation_.load()) {
        return false;  // switched off while the frame was being measured
    }
    const auto in_session = camera_ && active_camera_ && camera_generation == camera_generation_
        && frame.width == exposure_.frame_width && frame.height == exposure_.frame_height;
    if (!in_session || (exposure_.caps.knobs.empty() && !exposure_.caps.metering_region)) {
        return false;
    }
    const auto now = Clock::now();
    update_metering_region(*camera_, exposure_, region, frame, now);
    if (exposure_.caps.knobs.empty()) {
        return false;
    }
    const auto step = detail::next_exposure_step(exposure_.loop, stats, exposure_.caps.knobs,
        std::chrono::duration_cast<detail::ExposureTime>(now.time_since_epoch()));
    const auto changed = apply_step(*camera_, exposure_, step);
    log_decision(exposure_, stats, now);
    return changed;
}

}  // namespace su::recognizer
