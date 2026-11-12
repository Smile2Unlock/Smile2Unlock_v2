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
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
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
        probe_knobs();
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

    bool set_level(const ExposureKnobKind kind, const int level) {
        auto& slot = slots_[std::to_underlying(kind)];
        if (!slot) {
            return false;
        }
        if (kind == ExposureKnobKind::manual_exposure && !slot->touched) {
            // Switching auto exposure off is deferred to the first change so a
            // user who never needs it keeps the camera's own exposure. Marked
            // touched first so a half-applied switch is still undone.
            slot->touched = true;
            if (!write_int(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL)) {
                return false;
            }
        }
        slot->touched = true;
        return write_int(slot->cid, level);
    }

    // Puts one knob back to its value at open (for manual exposure that
    // re-enables the camera's auto exposure). No-op when it was never touched.
    bool reset_knob(const ExposureKnobKind kind) {
        auto& slot = slots_[std::to_underlying(kind)];
        if (!slot) {
            return false;
        }
        if (!std::exchange(slot->touched, false)) {
            return true;
        }
        return restore_slot(*slot);
    }

    // Restores everything touched, in reverse order. Safe to call repeatedly.
    void restore() {
        if (fd_ < 0) {
            return;
        }
        for (const auto kind : {ExposureKnobKind::manual_exposure,
                 ExposureKnobKind::backlight_compensation, ExposureKnobKind::ev_bias}) {
            (void)reset_knob(kind);
        }
        if (roi_touched_) {
            restore_int("roi_auto", V4L2_CID_UVC_REGION_OF_INTEREST_AUTO,
                static_cast<int>(roi_auto_original_));
            restore_rect();
        }
        roi_touched_ = false;
        fd_ = -1;
        slots_ = {};
        caps_ = {};
    }

private:
    struct Range {
        int min = 0;
        int max = 0;
        int step = 1;
        int current = 0;
    };

    // One drivable knob and whether the loop changed it (so it needs undoing).
    struct KnobSlot {
        ExposureKnob knob;
        std::uint32_t cid = 0;
        bool touched = false;
    };

    bool restore_slot(const KnobSlot& slot) const {
        if (slot.knob.kind != ExposureKnobKind::manual_exposure) {
            return restore_int("level", slot.cid, slot.knob.initial);
        }
        // Absolute exposure is only writable while auto is off, so it goes first.
        const auto exposure_ok =
            restore_int("exposure_absolute", V4L2_CID_EXPOSURE_ABSOLUTE, exposure_original_);
        return restore_int("exposure_auto", V4L2_CID_EXPOSURE_AUTO, exposure_auto_original_)
            && exposure_ok;
    }

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

    // Every usable knob, so the loop can escalate when one saturates.
    void probe_knobs() {
        if (const auto bias = query_range(V4L2_CID_AUTO_EXPOSURE_BIAS)) {
            use_knob(ExposureKnobKind::ev_bias, V4L2_CID_AUTO_EXPOSURE_BIAS, *bias);
        }
        if (const auto backlight = query_range(V4L2_CID_BACKLIGHT_COMPENSATION)) {
            use_knob(ExposureKnobKind::backlight_compensation,
                V4L2_CID_BACKLIGHT_COMPENSATION, *backlight);
        }
        probe_manual_exposure();
        const auto knob_of = [&](const ExposureKnobKind kind) {
            return slots_[std::to_underlying(kind)].transform(
                [](const KnobSlot& slot) { return slot.knob; });
        };
        caps_.knobs = make_knob_chain(knob_of(ExposureKnobKind::ev_bias),
            knob_of(ExposureKnobKind::backlight_compensation),
            knob_of(ExposureKnobKind::manual_exposure));
    }

    void use_knob(const ExposureKnobKind kind, const std::uint32_t cid, const Range& range) {
        slots_[std::to_underlying(kind)] = KnobSlot{
            .knob = {.kind = kind, .min = range.min, .max = range.max,
                     .step = std::max(1, range.step), .initial = range.current},
            .cid = cid};
    }

    void probe_manual_exposure() {
        const auto mode = query_range(V4L2_CID_EXPOSURE_AUTO, /*require_active=*/false);
        const auto exposure = query_range(V4L2_CID_EXPOSURE_ABSOLUTE, /*require_active=*/false);
        if (!mode || !exposure || mode->min > V4L2_EXPOSURE_MANUAL
            || mode->max < V4L2_EXPOSURE_MANUAL) {
            return;
        }
        exposure_auto_original_ = mode->current;
        exposure_original_ = exposure->current;
        const auto max = std::min(exposure->max, kMaxManualExposure);
        const auto min = std::min(std::max(exposure->min, 1), max);
        use_knob(ExposureKnobKind::manual_exposure, V4L2_CID_EXPOSURE_ABSOLUTE,
            Range{.min = min, .max = max, .step = exposure->step,
                  .current = std::clamp(exposure->current, min, max)});
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

    bool restore_int(const char* what, const std::uint32_t cid, const int value) const {
        if (write_int(cid, value)) {
            return true;
        }
        log_restore_failed(what, errno);
        return false;
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
        const auto knob_text = [](const ExposureKnob& knob) {
            return std::string{to_string(knob.kind)} + ":" + std::to_string(knob.min) + ".."
                + std::to_string(knob.max) + "@" + std::to_string(knob.initial);
        };
        const auto chain = caps_.knobs | std::views::transform(knob_text)
            | std::views::join_with(std::string_view{" > "}) | std::ranges::to<std::string>();
        su::log::process_logger().info("camera.exposure_probe", {
            su::log::field("metering_region", caps_.metering_region ? 1 : 0),
            su::log::field("knobs", chain.empty() ? std::string{"none"} : chain)});
    }

    int fd_ = -1;
    int frame_width_ = 0;
    int frame_height_ = 0;
    ExposureCapabilities caps_{};
    std::array<std::optional<KnobSlot>, kExposureKnobKindCount> slots_{};
    std::optional<v4l2_rect> roi_space_;
    v4l2_rect roi_rect_original_{};
    unsigned roi_auto_original_ = 0;
    int exposure_auto_original_ = 0;
    int exposure_original_ = 0;
    bool roi_touched_ = false;
};

}  // namespace su::recognizer::detail
