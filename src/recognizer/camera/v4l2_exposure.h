#pragma once
// V4L2 exposure controls for the backlight-aware loop (issue #137).
//
// UVC cameras keep their control values in firmware, so a change made here
// outlives this process and would leak into other apps. Every control that is
// touched is therefore remembered at probe time and put back by restore().

#include <linux/uvcvideo.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "common/log/logger.h"
#include "recognizer/exposure/exposure_control.h"

namespace su::recognizer::detail {

// Longest exposure the manual fallback may choose, in V4L2 100 microsecond
// units (1/30 s). Longer exposures drop the frame rate and blur the face.
inline constexpr int kMaxManualExposure = 333;

class V4l2ExposureControl {
public:
    // Reads the camera's capabilities and current values. Never fails: an
    // unsupported control just leaves that capability off.
    void probe(const int fd, const int frame_width, const int frame_height) {
        fd_ = fd;
        frame_width_ = frame_width;
        frame_height_ = frame_height;
        probe_roi();
        probe_knob();
        log_probe();
    }

    [[nodiscard]] ExposureCapabilities capabilities() const { return caps_; }

    bool set_region(const PixelRegion region) {
        if (!caps_.metering_region || !roi_space_ || region.width <= 0 || region.height <= 0) {
            return false;
        }
        const auto rect = to_sensor_rect(region);
        const auto wanted_auto = roi_auto_original_ | V4L2_UVC_REGION_OF_INTEREST_AUTO_EXPOSURE;
        // Rect first, then enable it, so the camera never meters a stale rect.
        roi_touched_ = true;
        return write_rect(V4L2_CID_UVC_REGION_OF_INTEREST_RECT, rect)
            && write_int(V4L2_CID_UVC_REGION_OF_INTEREST_AUTO, static_cast<int>(wanted_auto));
    }

    bool set_level(const int level) {
        if (!caps_.knob) {
            return false;
        }
        if (caps_.knob->kind == ExposureKnobKind::manual_exposure && !manual_engaged_) {
            // Switching auto exposure off is deferred to the first change so a
            // user who never needs it keeps the camera's own exposure.
            manual_engaged_ = true;
            if (!write_int(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL)) {
                return false;
            }
        }
        level_touched_ = true;
        return write_int(level_cid_, level);
    }

    // Restores everything touched, in reverse order. Safe to call repeatedly.
    void restore() {
        if (fd_ < 0) {
            return;
        }
        if (manual_engaged_) {
            // Absolute exposure is only writable while auto is off.
            restore_int("exposure_absolute", V4L2_CID_EXPOSURE_ABSOLUTE, exposure_original_);
            restore_int("exposure_auto", V4L2_CID_EXPOSURE_AUTO, exposure_auto_original_);
        } else if (level_touched_ && caps_.knob) {
            restore_int("level", level_cid_, caps_.knob->initial);
        }
        if (roi_touched_) {
            restore_int("roi_auto", V4L2_CID_UVC_REGION_OF_INTEREST_AUTO,
                static_cast<int>(roi_auto_original_));
            restore_rect();
        }
        manual_engaged_ = false;
        level_touched_ = false;
        roi_touched_ = false;
        fd_ = -1;
        caps_ = {};
    }

private:
    struct Range {
        int min = 0;
        int max = 0;
        int step = 1;
        int current = 0;
    };

    void probe_roi() {
        auto query = v4l2_query_ext_ctrl{};
        query.id = V4L2_CID_UVC_REGION_OF_INTEREST_RECT;
        auto auto_query = v4l2_query_ext_ctrl{};
        auto_query.id = V4L2_CID_UVC_REGION_OF_INTEREST_AUTO;
        if (ioctl(fd_, VIDIOC_QUERY_EXT_CTRL, &query) < 0 || query.type != V4L2_CTRL_TYPE_RECT
            || ioctl(fd_, VIDIOC_QUERY_EXT_CTRL, &auto_query) < 0
            || (auto_query.maximum & V4L2_UVC_REGION_OF_INTEREST_AUTO_EXPOSURE) == 0) {
            return;
        }
        const auto original = read_rect(V4L2_CTRL_WHICH_CUR_VAL);
        const auto bounds = read_rect(V4L2_CTRL_WHICH_MAX_VAL);
        const auto auto_original = read_int(V4L2_CID_UVC_REGION_OF_INTEREST_AUTO);
        if (!original || !auto_original) {
            return;
        }
        roi_rect_original_ = *original;
        roi_auto_original_ = static_cast<unsigned>(*auto_original);
        // The camera meters in its own coordinates; the maximum rect is the
        // whole sensor. Without it, assume the frame and the sensor coincide.
        roi_space_ = bounds && bounds->width > 0 && bounds->height > 0
            ? *bounds
            : v4l2_rect{0, 0, static_cast<__u32>(frame_width_), static_cast<__u32>(frame_height_)};
        caps_.metering_region = true;
    }

    void probe_knob() {
        if (const auto bias = query_range(V4L2_CID_AUTO_EXPOSURE_BIAS)) {
            use_knob(ExposureKnobKind::ev_bias, V4L2_CID_AUTO_EXPOSURE_BIAS, *bias);
        } else if (const auto backlight = query_range(V4L2_CID_BACKLIGHT_COMPENSATION)) {
            use_knob(ExposureKnobKind::backlight_compensation,
                V4L2_CID_BACKLIGHT_COMPENSATION, *backlight);
        } else {
            probe_manual_exposure();
        }
    }

    void use_knob(const ExposureKnobKind kind, const std::uint32_t cid, const Range& range) {
        level_cid_ = cid;
        caps_.knob = ExposureKnob{.kind = kind, .min = range.min, .max = range.max,
            .step = std::max(1, range.step), .initial = range.current};
    }

    bool probe_manual_exposure() {
        const auto mode = query_range(V4L2_CID_EXPOSURE_AUTO, /*require_active=*/false);
        const auto exposure = query_range(V4L2_CID_EXPOSURE_ABSOLUTE, /*require_active=*/false);
        if (!mode || !exposure || mode->min > V4L2_EXPOSURE_MANUAL
            || mode->max < V4L2_EXPOSURE_MANUAL) {
            return false;
        }
        exposure_auto_original_ = mode->current;
        exposure_original_ = exposure->current;
        const auto max = std::min(exposure->max, kMaxManualExposure);
        const auto min = std::min(std::max(exposure->min, 1), max);
        use_knob(ExposureKnobKind::manual_exposure, V4L2_CID_EXPOSURE_ABSOLUTE,
            Range{.min = min, .max = max, .step = exposure->step,
                  .current = std::clamp(exposure->current, min, max)});
        return true;
    }

    // Range of an integer control, or nullopt when it is missing, disabled
    // (and, unless told otherwise, inactive).
    std::optional<Range> query_range(const std::uint32_t cid, const bool require_active = true) {
        auto query = v4l2_queryctrl{};
        query.id = cid;
        constexpr auto kUnusable = V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY;
        if (ioctl(fd_, VIDIOC_QUERYCTRL, &query) < 0 || (query.flags & kUnusable) != 0
            || (require_active && (query.flags & V4L2_CTRL_FLAG_INACTIVE) != 0)
            || query.maximum <= query.minimum) {
            return std::nullopt;
        }
        const auto current = read_int(cid);
        if (!current) {
            return std::nullopt;
        }
        return Range{.min = query.minimum, .max = query.maximum, .step = query.step,
                     .current = *current};
    }

    std::optional<int> read_int(const std::uint32_t cid) const {
        auto control = v4l2_control{.id = cid, .value = 0};
        if (ioctl(fd_, VIDIOC_G_CTRL, &control) < 0) {
            return std::nullopt;
        }
        return control.value;
    }

    bool write_int(const std::uint32_t cid, const int value) const {
        auto control = v4l2_control{.id = cid, .value = value};
        return ioctl(fd_, VIDIOC_S_CTRL, &control) == 0;
    }

    std::optional<v4l2_rect> read_rect(const std::uint32_t which) const {
        auto rect = v4l2_rect{};
        auto control = v4l2_ext_control{};
        control.id = V4L2_CID_UVC_REGION_OF_INTEREST_RECT;
        control.size = sizeof(rect);
        control.ptr = &rect;
        auto controls = v4l2_ext_controls{};
        controls.which = which;
        controls.count = 1;
        controls.controls = &control;
        if (ioctl(fd_, VIDIOC_G_EXT_CTRLS, &controls) < 0) {
            return std::nullopt;
        }
        return rect;
    }

    bool write_rect(const std::uint32_t cid, v4l2_rect rect) const {
        auto control = v4l2_ext_control{};
        control.id = cid;
        control.size = sizeof(rect);
        control.ptr = &rect;
        auto controls = v4l2_ext_controls{};
        controls.which = V4L2_CTRL_WHICH_CUR_VAL;
        controls.count = 1;
        controls.controls = &control;
        return ioctl(fd_, VIDIOC_S_EXT_CTRLS, &controls) == 0;
    }

    // Frame pixel region -> the camera's metering coordinates.
    v4l2_rect to_sensor_rect(const PixelRegion region) const {
        const auto scale = [](const int value, const std::uint32_t space, const int frame) {
            return static_cast<int>(static_cast<long long>(value) * space / std::max(1, frame));
        };
        const auto space = *roi_space_;
        return v4l2_rect{
            .left = space.left + scale(region.x, space.width, frame_width_),
            .top = space.top + scale(region.y, space.height, frame_height_),
            .width = static_cast<__u32>(std::max(1, scale(region.width, space.width, frame_width_))),
            .height = static_cast<__u32>(std::max(1, scale(region.height, space.height, frame_height_))),
        };
    }

    void restore_int(const char* what, const std::uint32_t cid, const int value) const {
        if (!write_int(cid, value)) {
            log_restore_failed(what, errno);
        }
    }

    void restore_rect() const {
        if (!write_rect(V4L2_CID_UVC_REGION_OF_INTEREST_RECT, roi_rect_original_)) {
            log_restore_failed("roi_rect", errno);
        }
    }

    static void log_restore_failed(const char* what, const int error) {
        su::log::process_logger().warn("camera.exposure_restore_failed", {
            su::log::field("control", what), su::log::field("errno", error)});
    }

    void log_probe() const {
        const auto knob = caps_.knob;
        su::log::process_logger().info("camera.exposure_probe", {
            su::log::field("metering_region", caps_.metering_region ? 1 : 0),
            su::log::field("knob", knob ? to_string(knob->kind) : std::string_view{"none"}),
            su::log::field("min", knob ? knob->min : 0),
            su::log::field("max", knob ? knob->max : 0),
            su::log::field("initial", knob ? knob->initial : 0)});
    }

    int fd_ = -1;
    int frame_width_ = 0;
    int frame_height_ = 0;
    ExposureCapabilities caps_{};
    std::uint32_t level_cid_ = 0;
    std::optional<v4l2_rect> roi_space_;
    v4l2_rect roi_rect_original_{};
    unsigned roi_auto_original_ = 0;
    int exposure_auto_original_ = 0;
    int exposure_original_ = 0;
    bool roi_touched_ = false;
    bool level_touched_ = false;
    bool manual_engaged_ = false;
};

}  // namespace su::recognizer::detail
