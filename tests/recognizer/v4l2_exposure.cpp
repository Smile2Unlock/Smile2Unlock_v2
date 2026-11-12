#ifdef NDEBUG
#undef NDEBUG
#endif

// Unit tests of the V4L2 exposure controls against a fake device: every
// ioctl goes through an injected function object, so no camera is needed.

#include <linux/videodev2.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "recognizer/camera/v4l2_exposure.h"

using namespace su::recognizer::detail;

namespace {

constexpr int kFd = 7;
constexpr int kFrameWidth = 640;
constexpr int kFrameHeight = 480;
constexpr std::uint32_t kRoiRectCid = V4L2_CID_UVC_REGION_OF_INTEREST_RECT;
constexpr std::uint32_t kRoiAutoCid = V4L2_CID_UVC_REGION_OF_INTEREST_AUTO;
constexpr std::uint32_t kAutoCid = V4L2_CID_EXPOSURE_AUTO;
constexpr std::uint32_t kAbsoluteCid = V4L2_CID_EXPOSURE_ABSOLUTE;
constexpr std::uint32_t kBacklightCid = V4L2_CID_BACKLIGHT_COMPENSATION;
constexpr std::uint32_t kBiasCid = V4L2_CID_AUTO_EXPOSURE_BIAS;
constexpr int kEndlessFailures = -1;

struct Control {
    int min = 0;
    int max = 0;
    int step = 1;
    int value = 0;
    std::uint32_t type = V4L2_CTRL_TYPE_INTEGER;
    // Written values are rounded down to a multiple of this (driver snapping).
    int snap = 1;
};

struct Write {
    std::uint32_t cid;
    int value;
    bool operator==(const Write&) const = default;
};

// Fake UVC camera: a table of controls plus scripted errno failures.
class FakeDevice {
public:
    explicit FakeDevice(const bool roi_auto_on = false) {
        controls[kBacklightCid] = {.min = 0, .max = 2, .value = 0};
        controls[kAutoCid] = {.min = 0, .max = 3, .value = V4L2_EXPOSURE_APERTURE_PRIORITY,
                              .type = V4L2_CTRL_TYPE_MENU};
        controls[kAbsoluteCid] = {.min = 1, .max = 5000, .value = 156};
        controls[kRoiAutoCid] = {.min = 0, .max = V4L2_UVC_REGION_OF_INTEREST_AUTO_EXPOSURE,
            .value = roi_auto_on ? V4L2_UVC_REGION_OF_INTEREST_AUTO_EXPOSURE : 0};
        rect = {.left = 10, .top = 20, .width = 100, .height = 200};
    }

    int operator()(const int fd, const unsigned long request, void* arg) {
        assert(fd == kFd);
        errno = 0;
        switch (request) {
        case VIDIOC_QUERYCTRL: return query_control(static_cast<v4l2_queryctrl*>(arg));
        case VIDIOC_QUERY_EXT_CTRL: return query_ext(static_cast<v4l2_query_ext_ctrl*>(arg));
        case VIDIOC_QUERYMENU: return query_menu(static_cast<v4l2_querymenu*>(arg));
        case VIDIOC_G_CTRL: return get_control(static_cast<v4l2_control*>(arg));
        case VIDIOC_S_CTRL: return set_control(static_cast<v4l2_control*>(arg));
        case VIDIOC_G_EXT_CTRLS: return get_rect(static_cast<v4l2_ext_controls*>(arg));
        case VIDIOC_S_EXT_CTRLS: return set_rect(static_cast<v4l2_ext_controls*>(arg));
        default: errno = ENOTTY; return -1;
        }
    }

    // The next `times` writes of `cid` fail with `error` (kEndlessFailures: all).
    void fail_writes(const std::uint32_t cid, const int error, const int times = 1) {
        failures[cid].assign(times == kEndlessFailures ? 1000 : times, error);
    }

    std::map<std::uint32_t, Control> controls;
    std::map<std::uint32_t, std::set<int>> valid_menu_indices;
    std::map<std::uint32_t, std::deque<int>> failures;
    v4l2_rect rect{};
    std::vector<Write> writes;       // successful writes, in order
    int write_attempts = 0;          // every S_CTRL / S_EXT_CTRLS, failed or not

    [[nodiscard]] int value_of(const std::uint32_t cid) const { return controls.at(cid).value; }

    [[nodiscard]] std::size_t index_of(const std::uint32_t cid) const {
        return static_cast<std::size_t>(std::ranges::find_if(
            writes, [&](const Write& write) { return write.cid == cid; }) - writes.begin());
    }

private:
    int fail_if_scripted(const std::uint32_t cid) {
        ++write_attempts;
        auto found = failures.find(cid);
        if (found == failures.end() || found->second.empty()) {
            return 0;
        }
        errno = found->second.front();
        found->second.pop_front();
        return -1;
    }

    int query_control(v4l2_queryctrl* query) {
        const auto found = controls.find(query->id);
        if (found == controls.end()) {
            errno = EINVAL;
            return -1;
        }
        query->minimum = found->second.min;
        query->maximum = found->second.max;
        query->step = found->second.step;
        query->type = found->second.type;
        query->flags = 0;
        return 0;
    }

    int query_ext(v4l2_query_ext_ctrl* query) {
        if (query->id == kRoiRectCid) {
            query->type = V4L2_CTRL_TYPE_RECT;
            return 0;
        }
        const auto found = controls.find(query->id);
        if (found == controls.end()) {
            errno = EINVAL;
            return -1;
        }
        query->maximum = found->second.max;
        return 0;
    }

    int query_menu(v4l2_querymenu* menu) {
        const auto found = valid_menu_indices.find(menu->id);
        if (found == valid_menu_indices.end() || !found->second.contains(static_cast<int>(menu->index))) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }

    int get_control(v4l2_control* control) {
        const auto found = controls.find(control->id);
        if (found == controls.end()) {
            errno = EINVAL;
            return -1;
        }
        control->value = found->second.value;
        return 0;
    }

    int set_control(v4l2_control* control) {
        if (fail_if_scripted(control->id) < 0) {
            return -1;
        }
        auto& stored = controls.at(control->id);
        stored.value = control->value / stored.snap * stored.snap;
        writes.push_back({control->id, stored.value});
        return 0;
    }

    int get_rect(v4l2_ext_controls* controls_arg) {
        auto* out = static_cast<v4l2_rect*>(controls_arg->controls[0].ptr);
        *out = controls_arg->which == V4L2_CTRL_WHICH_MAX_VAL
            ? v4l2_rect{.left = 0, .top = 0, .width = 1920, .height = 1080}
            : rect;
        return 0;
    }

    int set_rect(v4l2_ext_controls* controls_arg) {
        if (fail_if_scripted(kRoiRectCid) < 0) {
            return -1;
        }
        rect = *static_cast<v4l2_rect*>(controls_arg->controls[0].ptr);
        writes.push_back({kRoiRectCid, rect.left});
        return 0;
    }
};

struct Rig {
    explicit Rig(const bool roi_auto_on = false) : device(roi_auto_on) {}

    // Probes against the fake. Declared after `device` so it outlives the use.
    [[nodiscard]] auto make_control() {
        auto control = BasicV4l2ExposureControl{[this](const int fd, const unsigned long request,
                                                        void* arg) { return device(fd, request, arg); }};
        control.probe(kFd, kFrameWidth, kFrameHeight);
        return control;
    }

    FakeDevice device;
};

constexpr auto kManual = ExposureKnobKind::manual_exposure;
constexpr auto kBacklight = ExposureKnobKind::backlight_compensation;

void test_probe_builds_the_chain() {
    auto rig = Rig{};
    rig.device.controls[kBiasCid] = {.min = 0, .max = 4, .value = 2,
                                     .type = V4L2_CTRL_TYPE_INTEGER_MENU};
    rig.device.valid_menu_indices[kBiasCid] = {0, 1, 2, 3, 4};
    auto control = rig.make_control();
    const auto caps = control.capabilities();
    assert(caps.metering_region);
    assert(caps.knobs.size() == 3);
    assert(caps.knobs[0].kind == ExposureKnobKind::ev_bias);
    assert(caps.knobs[2].kind == kManual && caps.knobs[2].max == kMaxManualExposure);
}

void test_sparse_ev_bias_menu_is_skipped() {
    auto rig = Rig{};
    rig.device.controls[kBiasCid] = {.min = 0, .max = 4, .value = 2,
                                     .type = V4L2_CTRL_TYPE_INTEGER_MENU};
    rig.device.valid_menu_indices[kBiasCid] = {0, 1, 3, 4};  // index 2 is a hole
    auto control = rig.make_control();
    assert(control.capabilities().knobs.size() == 2);
    assert(control.capabilities().knobs[0].kind == kBacklight);
}

void test_set_level_returns_the_applied_level() {
    auto rig = Rig{};
    rig.device.controls[kBacklightCid].snap = 2;
    auto control = rig.make_control();
    const auto applied = control.set_level(kBacklight, 3);
    assert(applied && *applied == 2);  // 3 snapped down to the driver's grid
    assert(rig.device.value_of(kBacklightCid) == 2);
    const auto missing = control.set_level(ExposureKnobKind::ev_bias, 1);
    assert(!missing && missing.error() == ExposureWriteError::unsupported);
}

void test_touched_failed_reset_is_retried_by_close() {
    auto rig = Rig{};
    auto control = rig.make_control();
    assert(control.set_level(kManual, 200));
    assert(rig.device.value_of(kAutoCid) == V4L2_EXPOSURE_MANUAL);

    // The reset fails once while re-enabling auto exposure: the knob must
    // stay touched, or close would leave the camera in manual exposure.
    rig.device.fail_writes(kAutoCid, EBUSY);
    const auto reset = control.reset_knob(kManual);
    assert(!reset && reset.error() == ExposureWriteError::transient);
    assert(rig.device.value_of(kAutoCid) == V4L2_EXPOSURE_MANUAL);

    control.restore();  // the close path retries
    assert(rig.device.value_of(kAutoCid) == V4L2_EXPOSURE_APERTURE_PRIORITY);
    assert(rig.device.value_of(kAbsoluteCid) == 156);
}

void test_successful_reset_is_not_repeated() {
    auto rig = Rig{};
    auto control = rig.make_control();
    assert(control.set_level(kBacklight, 2));
    assert(control.reset_knob(kBacklight));
    const auto writes_after_reset = rig.device.writes.size();
    assert(control.reset_knob(kBacklight));  // untouched again: no-op
    control.restore();
    assert(rig.device.writes.size() == writes_after_reset);
    assert(rig.device.value_of(kBacklightCid) == 0);
}

void test_restore_writes_exposure_before_auto_mode() {
    auto rig = Rig{};
    auto control = rig.make_control();
    assert(control.set_level(kManual, 200));
    rig.device.writes.clear();
    control.restore();
    assert(rig.device.index_of(kAbsoluteCid) < rig.device.index_of(kAutoCid));
    assert(rig.device.writes.size() == 2);
}

void test_restore_orders_roi_rect_and_auto_bit() {
    for (const auto auto_was_on : {true, false}) {
        auto rig = Rig{auto_was_on};
        auto control = rig.make_control();
        assert(control.set_region({.x = 100, .y = 100, .width = 200, .height = 200}));
        rig.device.writes.clear();
        control.restore();
        const auto rect_first = rig.device.index_of(kRoiRectCid) < rig.device.index_of(kRoiAutoCid);
        // Auto bit on at open: put the rect back first so re-enabling metering
        // never uses the stale rect. Off at open: stop metering, then move it.
        assert(rect_first == auto_was_on);
        assert(rig.device.rect.left == 10 && rig.device.rect.width == 100);
        assert(rig.device.value_of(kRoiAutoCid) == (auto_was_on ? 1 : 0));
    }
}

void test_partial_failure_still_restores_the_rest() {
    auto rig = Rig{};
    auto control = rig.make_control();
    assert(control.set_level(kManual, 200));
    assert(control.set_level(kBacklight, 2));
    assert(control.set_region({.x = 100, .y = 100, .width = 200, .height = 200}));
    rig.device.fail_writes(kAbsoluteCid, EIO, kEndlessFailures);
    control.restore();
    assert(rig.device.value_of(kAutoCid) == V4L2_EXPOSURE_APERTURE_PRIORITY);
    assert(rig.device.value_of(kBacklightCid) == 0);
    assert(rig.device.rect.left == 10);
    assert(rig.device.value_of(kRoiAutoCid) == 0);
}

void test_eintr_is_retried() {
    auto rig = Rig{};
    auto control = rig.make_control();
    rig.device.fail_writes(kBacklightCid, EINTR, 3);
    const auto applied = control.set_level(kBacklight, 1);
    assert(applied && *applied == 1);
    assert(rig.device.write_attempts == 4);

    // An endless EINTR storm gives up as transient instead of spinning.
    rig.device.fail_writes(kBacklightCid, EINTR, kEndlessFailures);
    const auto stormed = control.set_level(kBacklight, 2);
    assert(!stormed && stormed.error() == ExposureWriteError::transient);
}

void test_errno_classification() {
    for (const auto& [error, expected] : {
             std::pair{EBUSY, ExposureWriteError::transient},
             std::pair{EAGAIN, ExposureWriteError::transient},
             std::pair{EIO, ExposureWriteError::transient},
             std::pair{EINVAL, ExposureWriteError::rejected},
             std::pair{ENOTTY, ExposureWriteError::rejected},
             std::pair{EACCES, ExposureWriteError::rejected}}) {
        auto rig = Rig{};
        auto control = rig.make_control();
        rig.device.fail_writes(kBacklightCid, error);
        const auto result = control.set_level(kBacklight, 1);
        assert(!result && result.error() == expected);
    }
    auto rig = Rig{};
    auto control = rig.make_control();
    rig.device.fail_writes(kRoiRectCid, EINVAL);
    const auto region = control.set_region({.x = 100, .y = 100, .width = 200, .height = 200});
    assert(!region && region.error() == ExposureWriteError::rejected);
}

}  // namespace

int main() {
    test_probe_builds_the_chain();
    test_sparse_ev_bias_menu_is_skipped();
    test_set_level_returns_the_applied_level();
    test_touched_failed_reset_is_retried_by_close();
    test_successful_reset_is_not_repeated();
    test_restore_writes_exposure_before_auto_mode();
    test_restore_orders_roi_rect_and_auto_bit();
    test_partial_failure_still_restores_the_rest();
    test_eintr_is_retried();
    test_errno_classification();
    return 0;
}
