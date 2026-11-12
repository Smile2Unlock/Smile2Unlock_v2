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

void apply_level(V4L2Camera& camera, ExposureSession& session, const int level) {
    if (camera.set_exposure_level(level)) {
        return;
    }
    // The control rejected the value: stop driving it for this session
    // rather than retrying on every frame.
    session.caps.knob.reset();
    su::log::process_logger().warn("camera.exposure_knob_failed", {su::log::field("level", level)});
}

void log_decision(ExposureSession& session, const detail::LumaStats& stats,
    const Clock::time_point now) {
    if (now < session.next_log) {
        return;
    }
    session.next_log = now + kLogInterval;
    su::log::process_logger().debug("camera.exposure_observe", {
        su::log::field("mean", stats.mean), su::log::field("clipped", stats.clipped_fraction),
        su::log::field("level", session.loop.level),
        su::log::field("knob", session.caps.knob ? detail::to_string(session.caps.knob->kind) : std::string_view{"none"})});
}

}  // namespace

void RecognizerService::observe_exposure(
    const ImageView frame, const std::optional<FaceBox> face) const {
    std::lock_guard lock(camera_mutex_);
    if (!camera_ || !active_camera_ || (!exposure_.caps.knob && !exposure_.caps.metering_region)) {
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
    if (!exposure_.caps.knob) {
        return;
    }
    const auto step = detail::next_exposure_step(exposure_.loop, stats, *exposure_.caps.knob);
    exposure_.loop = step.next;
    if (step.apply_level) {
        apply_level(*camera_, exposure_, *step.apply_level);
    }
    log_decision(exposure_, stats, now);
}

}  // namespace su::recognizer
