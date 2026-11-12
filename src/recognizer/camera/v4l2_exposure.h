#pragma once
// V4L2 exposure controls for the backlight-aware loop (issue #137).
//
// UVC cameras keep their control values in firmware, so a change made here
// outlives this process and would leak into other apps. Every control that is
// touched is therefore remembered at probe time and put back by restore().
//
// The control class is templated on the ioctl function object so tests can run
// it against a fake device; production uses ::ioctl through SystemIoctl.

#include <linux/uvcvideo.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
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

// Upper bound on EINTR retries of one ioctl, so a signal storm cannot spin.
inline constexpr int kMaxInterruptedRetries = 16;

struct SystemIoctl {
    int operator()(const int fd, const unsigned long request, void* arg) const {
        return ::ioctl(fd, request, arg);
    }
};

// busy/again/io/interrupted failures may pass on their own; everything else
// (EINVAL, ENOTTY, EACCES, ...) means the camera refuses this control or value.
[[nodiscard]] constexpr ExposureWriteError classify_errno(const int error) {
    switch (error) {
    case EBUSY:
    case EAGAIN:
    case EIO:
    case EINTR: return ExposureWriteError::transient;
    default: return ExposureWriteError::rejected;
    }
}

using ExposureResult = std::expected<void, ExposureWriteError>;

template <typename Ioctl = SystemIoctl>
class BasicV4l2ExposureControl {
public:
    explicit BasicV4l2ExposureControl(Ioctl ioctl_fn = {}) : ioctl_(std::move(ioctl_fn)) {}

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

    [[nodiscard]] ExposureResult set_region(const PixelRegion region) {
        if (!caps_.metering_region || !roi_space_) {
            return std::unexpected(ExposureWriteError::unsupported);
        }
        if (region.width <= 0 || region.height <= 0) {
            return std::unexpected(ExposureWriteError::rejected);
        }
        const auto wanted_auto = static_cast<int>(
            roi_auto_original_ | V4L2_UVC_REGION_OF_INTEREST_AUTO_EXPOSURE);
        // Rect first, then enable it, so the camera never meters a stale rect.
        roi_touched_ = true;
        return checked_write(kRoiRectCid, region.x,
                   write_rect(kRoiRectCid, to_sensor_rect(region)))
            .and_then([&] {
                return checked_write(kRoiAutoCid, wanted_auto, write_int(kRoiAutoCid, wanted_auto));
            });
    }

    // Writes `level` and returns the level the camera applied (drivers snap
    // requests to their own grid).
    [[nodiscard]] std::expected<int, ExposureWriteError> set_level(
        const ExposureKnobKind kind, const int level) {
        auto& slot = slots_[std::to_underlying(kind)];
        if (!slot) {
            return std::unexpected(ExposureWriteError::unsupported);
        }
        return engage_manual_if_needed(*slot)
            .and_then([&] {
                slot->touched = true;
                return checked_write(slot->cid, level, write_int(slot->cid, level));
            })
            .transform([&] { return read_int(slot->cid).value_or(level); });
    }

    // Puts one knob back to its value at open (for manual exposure that
    // re-enables the camera's auto exposure). No-op when it was never touched.
    // The knob stays marked as touched until the restore succeeds, so a failed
    // reset is retried by restore() on close.
    [[nodiscard]] ExposureResult reset_knob(const ExposureKnobKind kind) {
        auto& slot = slots_[std::to_underlying(kind)];
        if (!slot) {
            return std::unexpected(ExposureWriteError::unsupported);
        }
        if (!slot->touched) {
            return {};
        }
        return restore_slot(*slot)
            .transform([&] { slot->touched = false; })
            .transform_error(classify_errno);
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
        if (roi_touched_ && restore_roi()) {
            roi_touched_ = false;
        }
        fd_ = -1;
        slots_ = {};
        caps_ = {};
    }

private:
    using ErrnoResult = std::expected<void, int>;
    static constexpr auto kRoiRectCid = V4L2_CID_UVC_REGION_OF_INTEREST_RECT;
    static constexpr auto kRoiAutoCid = V4L2_CID_UVC_REGION_OF_INTEREST_AUTO;
    static constexpr auto kAutoCid = V4L2_CID_EXPOSURE_AUTO;

    struct Range {
        int min = 0;
        int max = 0;
        int step = 1;
        int current = 0;
        std::uint32_t type = V4L2_CTRL_TYPE_INTEGER;
    };

    // One drivable knob and whether the loop changed it (so it needs undoing).
    struct KnobSlot {
        ExposureKnob knob;
        std::uint32_t cid = 0;
        bool touched = false;
    };

    // ioctl with EINTR retried; errno is the failure reason on return -1.
    int call(const unsigned long request, void* arg) const {
        auto result = ioctl_(fd_, request, arg);
        for (auto retries = 0; result < 0 && errno == EINTR && retries < kMaxInterruptedRetries;
             ++retries) {
            result = ioctl_(fd_, request, arg);
        }
        return result;
    }

    // Logs a failed loop write (errno, control and value) and classifies it.
    [[nodiscard]] static ExposureResult checked_write(
        const std::uint32_t cid, const int value, const ErrnoResult& written) {
        if (written) {
            return {};
        }
        const auto error = written.error();
        su::log::process_logger().warn("camera.exposure_write_failed", {
            su::log::field("control", cid), su::log::field("value", value),
            su::log::field("errno", error)});
        return std::unexpected(classify_errno(error));
    }

    // Absolute exposure is only effective once auto exposure is off. Deferred
    // to the first change so a user who never needs it keeps the camera's own
    // exposure; marked touched first so a half-applied switch is still undone.
    [[nodiscard]] ExposureResult engage_manual_if_needed(KnobSlot& slot) {
        if (slot.knob.kind != ExposureKnobKind::manual_exposure || slot.touched) {
            return {};
        }
        slot.touched = true;
        return checked_write(kAutoCid, V4L2_EXPOSURE_MANUAL,
            write_int(kAutoCid, V4L2_EXPOSURE_MANUAL));
    }

    // Every control is attempted even when an earlier one failed; the first
    // error is reported.
    [[nodiscard]] ErrnoResult restore_slot(const KnobSlot& slot) const {
        if (slot.knob.kind != ExposureKnobKind::manual_exposure) {
            return restore_int("level", slot.cid, slot.knob.initial);
        }
        // Absolute exposure is only writable while auto is off, so it goes first.
        const auto exposure =
            restore_int("exposure_absolute", V4L2_CID_EXPOSURE_ABSOLUTE, exposure_original_);
        const auto mode = restore_int("exposure_auto", kAutoCid, exposure_auto_original_);
        return exposure ? mode : exposure;
    }

    // Original auto bit set: put the rect back first, so re-enabling auto never
    // meters a stale rect. Clear: stop auto metering, then move the rect.
    [[nodiscard]] ErrnoResult restore_roi() const {
        const auto auto_was_on =
            (roi_auto_original_ & V4L2_UVC_REGION_OF_INTEREST_AUTO_EXPOSURE) != 0;
        const auto restore_rect_value = [&] { return restore_rect_control(); };
        const auto restore_auto_bit = [&] {
            return restore_int("roi_auto", kRoiAutoCid, static_cast<int>(roi_auto_original_));
        };
        const auto first = auto_was_on ? restore_rect_value() : restore_auto_bit();
        const auto second = auto_was_on ? restore_auto_bit() : restore_rect_value();
        return first ? second : first;
    }

    void probe_roi() {
        auto query = v4l2_query_ext_ctrl{};
        query.id = kRoiRectCid;
        auto auto_query = v4l2_query_ext_ctrl{};
        auto_query.id = kRoiAutoCid;
        if (call(VIDIOC_QUERY_EXT_CTRL, &query) < 0 || query.type != V4L2_CTRL_TYPE_RECT
            || call(VIDIOC_QUERY_EXT_CTRL, &auto_query) < 0
            || (auto_query.maximum & V4L2_UVC_REGION_OF_INTEREST_AUTO_EXPOSURE) == 0) {
            return;
        }
        const auto original = read_rect(V4L2_CTRL_WHICH_CUR_VAL);
        const auto bounds = read_rect(V4L2_CTRL_WHICH_MAX_VAL);
        const auto auto_original = read_int(kRoiAutoCid);
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
        if (const auto bias = query_range(V4L2_CID_AUTO_EXPOSURE_BIAS);
            bias && menu_is_dense(V4L2_CID_AUTO_EXPOSURE_BIAS, *bias)) {
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

    // EV bias is an integer menu: the loop steps through indices, which is
    // only sound when every index between min and max is a real entry. A
    // sparse menu would make a step land on a hole, so the knob is skipped.
    bool menu_is_dense(const std::uint32_t cid, const Range& range) const {
        if (range.type != V4L2_CTRL_TYPE_INTEGER_MENU && range.type != V4L2_CTRL_TYPE_MENU) {
            return true;
        }
        return std::ranges::all_of(std::views::iota(range.min, range.max + 1), [&](const int index) {
            auto entry = v4l2_querymenu{};
            entry.id = cid;
            entry.index = static_cast<__u32>(index);
            return call(VIDIOC_QUERYMENU, &entry) == 0;
        });
    }

    void use_knob(const ExposureKnobKind kind, const std::uint32_t cid, const Range& range) {
        slots_[std::to_underlying(kind)] = KnobSlot{
            .knob = {.kind = kind, .min = range.min, .max = range.max,
                     .step = std::max(1, range.step), .initial = range.current},
            .cid = cid};
    }

    void probe_manual_exposure() {
        const auto mode = query_range(kAutoCid, /*require_active=*/false);
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
        if (call(VIDIOC_QUERYCTRL, &query) < 0 || (query.flags & kUnusable) != 0
            || (require_active && (query.flags & V4L2_CTRL_FLAG_INACTIVE) != 0)
            || query.maximum <= query.minimum) {
            return std::nullopt;
        }
        const auto current = read_int(cid);
        if (!current) {
            return std::nullopt;
        }
        return Range{.min = query.minimum, .max = query.maximum, .step = query.step,
                     .current = *current, .type = query.type};
    }

    std::optional<int> read_int(const std::uint32_t cid) const {
        auto control = v4l2_control{.id = cid, .value = 0};
        if (call(VIDIOC_G_CTRL, &control) < 0) {
            return std::nullopt;
        }
        return control.value;
    }

    ErrnoResult write_int(const std::uint32_t cid, const int value) const {
        auto control = v4l2_control{.id = cid, .value = value};
        if (call(VIDIOC_S_CTRL, &control) == 0) {
            return {};
        }
        return std::unexpected(errno);
    }

    std::optional<v4l2_rect> read_rect(const std::uint32_t which) const {
        auto rect = v4l2_rect{};
        auto control = v4l2_ext_control{};
        control.id = kRoiRectCid;
        control.size = sizeof(rect);
        control.ptr = &rect;
        auto controls = v4l2_ext_controls{};
        controls.which = which;
        controls.count = 1;
        controls.controls = &control;
        if (call(VIDIOC_G_EXT_CTRLS, &controls) < 0) {
            return std::nullopt;
        }
        return rect;
    }

    ErrnoResult write_rect(const std::uint32_t cid, v4l2_rect rect) const {
        auto control = v4l2_ext_control{};
        control.id = cid;
        control.size = sizeof(rect);
        control.ptr = &rect;
        auto controls = v4l2_ext_controls{};
        controls.which = V4L2_CTRL_WHICH_CUR_VAL;
        controls.count = 1;
        controls.controls = &control;
        if (call(VIDIOC_S_EXT_CTRLS, &controls) == 0) {
            return {};
        }
        return std::unexpected(errno);
    }

    // Frame pixel region -> the camera's metering coordinates (64-bit: the
    // region is derived from external geometry).
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

    ErrnoResult restore_int(const char* what, const std::uint32_t cid, const int value) const {
        const auto written = write_int(cid, value);
        if (!written) {
            log_restore_failed(what, value, written.error());
        }
        return written;
    }

    ErrnoResult restore_rect_control() const {
        const auto written = write_rect(kRoiRectCid, roi_rect_original_);
        if (!written) {
            log_restore_failed("roi_rect", roi_rect_original_.left, written.error());
        }
        return written;
    }

    static void log_restore_failed(const char* what, const int value, const int error) {
        su::log::process_logger().warn("camera.exposure_restore_failed", {
            su::log::field("control", what), su::log::field("value", value),
            su::log::field("errno", error)});
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

    Ioctl ioctl_;
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

using V4l2ExposureControl = BasicV4l2ExposureControl<SystemIoctl>;

}  // namespace su::recognizer::detail
