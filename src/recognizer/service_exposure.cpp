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
    if (camera.set_metering_region(region)) {
        session.applied_region = region;
        return;
    }
    session.caps.metering_region = false;
    su::log::process_logger().warn("camera.exposure_region_failed", {});
}

bool write_change(V4L2Camera& camera, const detail::ExposureChange& change) {
    return change.action == detail::ExposureAction::set
        ? camera.set_exposure_level(change.kind, change.level)
        : camera.reset_exposure_knob(change.kind);
}

void apply_change(V4L2Camera& camera, ExposureSession& session, const detail::ExposureChange& change) {
    if (write_change(camera, change)) {
        return;
    }
    // The control rejected the value: drop just that knob from the chain for
    // this session (the loop then skips or escalates past it) rather than
    // retrying on every frame. Best-effort reset so a half-applied change,
    // above all manual exposure, does not stay on the camera.
    session.caps.knobs = session.caps.knobs.without(change.kind);
    if (change.action == detail::ExposureAction::set) {
        (void)camera.reset_exposure_knob(change.kind);
    }
    su::log::process_logger().warn("camera.exposure_knob_failed", {
        su::log::field("knob", detail::to_string(change.kind)),
        su::log::field("level", change.level)});
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

void RecognizerService::observe_exposure(
    const ImageView frame, const std::optional<FaceBox> face) const {
    std::lock_guard lock(camera_mutex_);
    if (!camera_ || !active_camera_ || (exposure_.caps.knobs.empty() && !exposure_.caps.metering_region)) {
        return;
    }
    const auto now = Clock::now();
    const auto face_region = face
        ? std::optional(detail::PixelRegion{face->x, face->y, face->width, face->height})
        : std::nullopt;
    const auto region = detail::metering_region(frame.width, frame.height, face_region);
    update_metering_region(*camera_, exposure_, region, frame, now);
    const auto stats = detail::measure_luma(
        frame.bytes, frame.width, frame.height, frame.channels, region);
    if (exposure_.caps.knobs.empty()) {
        return;
    }
    const auto step = detail::next_exposure_step(exposure_.loop, stats, exposure_.caps.knobs);
    exposure_.loop = step.next;
    if (step.change) {
        apply_change(*camera_, exposure_, *step.change);
    }
    log_decision(exposure_, stats, now);
}

}  // namespace su::recognizer
