#ifdef NDEBUG
#undef NDEBUG
#endif

// Pure-helper tests for the Windows camera exposure controls (issue #137).
// The COM side needs a real camera; the unit conversions and range mapping do not.

#include <cassert>
#include <cmath>

#include "recognizer/camera/windows_camera_controls.h"

namespace d = su::recognizer::detail;

namespace {

void log2_units_round_trip() {
    // DirectShow stores exposure as log2(seconds); -5 is 1/32 s = 312.5 * 100 us.
    assert(d::exposure_log2_to_units(-5) == 313);
    assert(d::exposure_log2_to_units(0) == 10000);
    assert(d::exposure_log2_to_units(-20) == 1);  // never below one unit
    for (long v = -10; v <= -2; ++v) {
        assert(d::exposure_units_to_log2(d::exposure_log2_to_units(v), -12, 0, 1) == v);
    }
}

void units_snap_to_nearest_log2_and_clamp() {
    // 200 units = 1/50 s is nearer 2^-6 (1/64) than 2^-5 (1/32) in the log domain.
    assert(d::exposure_units_to_log2(200, -11, -2, 1) == -6);
    assert(d::exposure_units_to_log2(250, -11, -2, 1) == -5);
    assert(d::exposure_units_to_log2(1, -8, -2, 1) == -8);          // below range
    assert(d::exposure_units_to_log2(1000000, -8, -2, 1) == -2);    // above range
    assert(d::exposure_units_to_log2(0, -8, -2, 1) == -8);          // non-positive
}

void snap_level_respects_step_and_range() {
    assert(d::snap_level_to_range(5, 0, 10, 2) == 6);
    assert(d::snap_level_to_range(-3, 0, 10, 2) == 0);
    assert(d::snap_level_to_range(99, 0, 10, 4) == 8);   // last reachable step
    assert(d::snap_level_to_range(7, 0, 10, 0) == 7);    // bogus step treated as 1
}

void backlight_knob_needs_manual_range() {
    const d::ProcAmpRange usable{0, 4, 1, 0, d::kProcAmpCapsManual | d::kProcAmpCapsAuto};
    const auto knob = d::make_backlight_knob(usable, 1);
    assert(knob && knob->kind == d::ExposureKnobKind::backlight_compensation);
    assert(knob->min == 0 && knob->max == 4 && knob->step == 1 && knob->initial == 1);

    assert(!d::make_backlight_knob({0, 0, 1, 0, d::kProcAmpCapsManual}, 0));  // no range
    assert(!d::make_backlight_knob({0, 4, 1, 0, d::kProcAmpCapsAuto}, 0));    // no manual
    assert(d::make_backlight_knob(usable, 40)->initial == 4);                 // clamped
}

void manual_exposure_knob_is_linear_and_capped() {
    const d::ProcAmpRange range{-11, -2, 1, -5, d::kProcAmpCapsManual};
    const auto knob = d::make_manual_exposure_knob(range, -6);
    assert(knob && knob->kind == d::ExposureKnobKind::manual_exposure);
    assert(knob->min == d::exposure_log2_to_units(-11));
    assert(knob->max == d::kMaxManualExposureUnits);  // -2 would be 1/4 s: capped at 1/30 s
    assert(knob->initial == d::exposure_log2_to_units(-6));
    assert(knob->step == 1);

    assert(!d::make_manual_exposure_knob({-11, -2, 1, -5, d::kProcAmpCapsAuto}, -6));
    // Every exposure the driver offers is slower than the cap.
    assert(!d::make_manual_exposure_knob({-2, -1, 1, -2, d::kProcAmpCapsManual}, -2));
}

void knob_preference_order() {
    const auto ev = d::ExposureKnob{.kind = d::ExposureKnobKind::ev_bias};
    const auto bl = d::ExposureKnob{.kind = d::ExposureKnobKind::backlight_compensation};
    const auto man = d::ExposureKnob{.kind = d::ExposureKnobKind::manual_exposure};
    assert(d::select_exposure_knob(ev, bl, man)->kind == d::ExposureKnobKind::ev_bias);
    assert(d::select_exposure_knob({}, bl, man)->kind
        == d::ExposureKnobKind::backlight_compensation);
    assert(d::select_exposure_knob({}, {}, man)->kind == d::ExposureKnobKind::manual_exposure);
    assert(!d::select_exposure_knob({}, {}, {}));
}

}  // namespace

int main() {
    log2_units_round_trip();
    units_snap_to_nearest_log2_and_clamp();
    snap_level_respects_step_and_range();
    backlight_knob_needs_manual_range();
    manual_exposure_knob_is_linear_and_capped();
    knob_preference_order();
    return 0;
}
