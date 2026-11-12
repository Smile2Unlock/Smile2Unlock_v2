#ifdef NDEBUG
#undef NDEBUG
#endif

// Pure-helper tests for the Windows camera exposure controls (issue #137).
// The COM side needs a real camera; the unit conversions and range mapping do not.

#include <windows.h>
#include <strmif.h>

#include <cassert>
#include <cmath>
#include <optional>
#include <vector>

#include "recognizer/camera/windows_camera_controls.h"
#include "recognizer/camera/windows_hresult.h"

namespace d = su::recognizer::detail;
using su::recognizer::CameraExposureControls;
using d::ExposureKnobKind;
using d::ExposureWriteError;

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

void knob_chain_keeps_preference_order() {
    const auto ev = d::ExposureKnob{.kind = d::ExposureKnobKind::ev_bias};
    const auto bl = d::ExposureKnob{.kind = d::ExposureKnobKind::backlight_compensation};
    const auto man = d::ExposureKnob{.kind = d::ExposureKnobKind::manual_exposure};
    const auto all = d::make_knob_chain(ev, bl, man);
    assert(all.size() == 3);
    assert(all[0].kind == d::ExposureKnobKind::ev_bias);
    assert(all[1].kind == d::ExposureKnobKind::backlight_compensation);
    assert(all[2].kind == d::ExposureKnobKind::manual_exposure);

    // What the Windows backend reports: backlight first, manual as escalation.
    const auto windows = d::make_knob_chain({}, bl, man);
    assert(windows.size() == 2 && windows[0].kind == bl.kind && windows[1].kind == man.kind);
    assert(d::make_knob_chain({}, {}, man).size() == 1);
    assert(d::make_knob_chain({}, {}, {}).empty());
}

void snapped_exposure_round_trips_to_linear_units() {
    // 200 units snaps to 2^-6 s; the loop must be told 156 (what the camera
    // really uses), not 200, or its anti-flip-flop memory compares wrong levels.
    const auto snapped = d::snap_exposure(200, -11, -2, 1);
    assert(snapped.log2_seconds == -6 && snapped.units == d::exposure_log2_to_units(-6));
    assert(snapped.units == 156);
    // A coarse driver grid (every 2nd exponent from -11) snaps further.
    const auto coarse = d::snap_exposure(200, -11, -2, 2);
    assert(coarse.units == d::exposure_log2_to_units(coarse.log2_seconds));
    assert((coarse.log2_seconds - -11) % 2 == 0);
    // The snapped value never exceeds the 1/30 s cap even when the grid would.
    const auto capped = d::snap_exposure(d::kMaxManualExposureUnits, -11, -2, 3);
    assert(capped.units <= d::kMaxManualExposureUnits);
    assert((capped.log2_seconds - -11) % 3 == 0);
}

void backlight_snaps_to_driver_step() {
    const auto knob = d::make_backlight_knob({0, 10, 4, 0, d::kProcAmpCapsManual}, 0);
    assert(knob);
    assert(d::snap_level_to_range(5, knob->min, knob->max, knob->step) == 4);
    assert(d::snap_level_to_range(7, knob->min, knob->max, knob->step) == 8);
    assert(d::snap_level_to_range(10, knob->min, knob->max, knob->step) == 8);
}

void hresult_classification() {
    assert(d::classify_hresult(HRESULT_FROM_WIN32(ERROR_BUSY)) == ExposureWriteError::transient);
    assert(d::classify_hresult(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))
        == ExposureWriteError::transient);
    assert(d::classify_hresult(MF_E_VIDEO_DEVICE_LOCKED) == ExposureWriteError::transient);
    assert(d::classify_hresult(MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED)
        == ExposureWriteError::transient);
    assert(d::classify_hresult(E_PENDING) == ExposureWriteError::transient);
    assert(d::classify_hresult(RPC_E_TIMEOUT) == ExposureWriteError::transient);
    assert(d::classify_hresult(RPC_E_CALL_REJECTED) == ExposureWriteError::transient);
    assert(d::classify_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT)) == ExposureWriteError::transient);
    assert(d::classify_hresult(E_NOINTERFACE) == ExposureWriteError::unsupported);
    assert(d::classify_hresult(E_NOTIMPL) == ExposureWriteError::unsupported);
    assert(d::classify_hresult(HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        == ExposureWriteError::unsupported);
    assert(d::classify_hresult(E_FAIL) == ExposureWriteError::rejected);
    assert(d::classify_hresult(E_INVALIDARG) == ExposureWriteError::rejected);
}

// Minimal COM fake of IAMVideoProcAmp / IAMCameraControl (identical vtable
// shape). Objects live on the stack, so Release never deletes.
struct SetCall {
    long value;
    long flags;
};

template <typename Interface>
class FakeControl final : public Interface {
public:
    long min = 0, max = 0, step = 1, def = 0, caps = d::kProcAmpCapsManual;
    long original_value = 0, original_flags = 0;
    int failing_sets = 0;  // next N Set calls fail with `failure`
    HRESULT failure = E_FAIL;
    std::vector<SetCall> sets;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == __uuidof(Interface)) {
            *out = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE GetRange(long, long* lo, long* hi, long* st, long* df,
        long* cp) override {
        *lo = min; *hi = max; *st = step; *df = def; *cp = caps;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Set(long, long value, long flags) override {
        sets.push_back({value, flags});
        if (failing_sets > 0) {
            --failing_sets;
            return failure;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Get(long, long* value, long* flags) override {
        *value = original_value;
        *flags = original_flags;
        return S_OK;
    }

private:
    ULONG refs_ = 0;
};

struct Rig {
    FakeControl<IAMVideoProcAmp> proc_amp;
    FakeControl<IAMCameraControl> camera;
    CameraExposureControls controls;

    Rig() {
        proc_amp.min = 0; proc_amp.max = 10; proc_amp.step = 2;
        proc_amp.original_value = 2;
        camera.min = -11; camera.max = -2; camera.step = 1;
        camera.original_value = -6;
        camera.original_flags = d::kProcAmpCapsAuto;
        camera.caps = d::kProcAmpCapsManual | d::kProcAmpCapsAuto;
        const auto caps = controls.probe(&proc_amp, &camera);
        assert(caps.knobs.size() == 2);
    }
};

void set_level_reports_the_applied_level() {
    Rig rig;
    const auto backlight = rig.controls.set_level(ExposureKnobKind::backlight_compensation, 5);  // step 2 rounds half up
    assert(backlight && *backlight == 6);
    assert(rig.proc_amp.sets.back().value == 6);
    const auto exposure = rig.controls.set_level(ExposureKnobKind::manual_exposure, 200);
    assert(exposure && *exposure == 156);
    assert(rig.camera.sets.back().value == -6);
    assert(rig.camera.sets.back().flags == CameraControl_Flags_Manual);
}

void set_level_maps_hresults() {
    Rig rig;
    rig.proc_amp.failing_sets = 3;
    rig.proc_amp.failure = HRESULT_FROM_WIN32(ERROR_BUSY);
    assert(rig.controls.set_level(ExposureKnobKind::backlight_compensation, 2).error()
        == ExposureWriteError::transient);
    rig.proc_amp.failure = E_NOINTERFACE;
    assert(rig.controls.set_level(ExposureKnobKind::backlight_compensation, 2).error()
        == ExposureWriteError::unsupported);
    rig.proc_amp.failure = E_FAIL;
    assert(rig.controls.set_level(ExposureKnobKind::backlight_compensation, 2).error()
        == ExposureWriteError::rejected);
    assert(rig.controls.set_level(ExposureKnobKind::ev_bias, 1).error()
        == ExposureWriteError::unsupported);
}

void failed_reset_keeps_control_marked_until_it_succeeds() {
    Rig rig;
    assert(rig.controls.set_level(ExposureKnobKind::manual_exposure, 200));
    rig.camera.failing_sets = 1;
    rig.camera.failure = HRESULT_FROM_WIN32(ERROR_BUSY);
    const auto failed = rig.controls.reset(ExposureKnobKind::manual_exposure);
    assert(!failed && failed.error() == ExposureWriteError::transient);
    // Still marked: the retry reaches the camera and restores value AND auto flag.
    assert(rig.controls.reset(ExposureKnobKind::manual_exposure));
    assert(rig.camera.sets.back().value == -6);
    assert(rig.camera.sets.back().flags == d::kProcAmpCapsAuto);
    const auto calls = rig.camera.sets.size();
    assert(rig.controls.reset(ExposureKnobKind::manual_exposure));  // cleared: no write
    assert(rig.camera.sets.size() == calls);
}

void restore_attempts_every_control_and_release_retries() {
    Rig rig;
    assert(rig.controls.set_level(ExposureKnobKind::backlight_compensation, 6));
    assert(rig.controls.set_level(ExposureKnobKind::manual_exposure, 200));
    rig.camera.failing_sets = 1;
    assert(!rig.controls.restore());
    // The failing exposure restore did not stop the backlight one.
    assert(rig.proc_amp.sets.back().value == 2);
    const auto proc_amp_calls = rig.proc_amp.sets.size();
    rig.controls.release();
    assert(rig.camera.sets.back().value == -6);               // retried and restored
    assert(rig.proc_amp.sets.size() == proc_amp_calls);       // already restored
}

void release_gives_up_after_bounded_retries() {
    Rig rig;
    assert(rig.controls.set_level(ExposureKnobKind::manual_exposure, 200));
    rig.camera.failing_sets = 1000;
    const auto before = rig.camera.sets.size();
    rig.controls.release();
    assert(rig.camera.sets.size() - before == su::recognizer::kRestoreAttempts);
}

void reset_of_unprobed_knob_is_unsupported() {
    CameraExposureControls controls;
    assert(controls.reset(ExposureKnobKind::manual_exposure).error()
        == ExposureWriteError::unsupported);
    assert(controls.reset(ExposureKnobKind::ev_bias).error() == ExposureWriteError::unsupported);
}

}  // namespace

int main() {
    snapped_exposure_round_trips_to_linear_units();
    backlight_snaps_to_driver_step();
    hresult_classification();
    set_level_reports_the_applied_level();
    set_level_maps_hresults();
    failed_reset_keeps_control_marked_until_it_succeeds();
    restore_attempts_every_control_and_release_retries();
    release_gives_up_after_bounded_retries();
    reset_of_unprobed_knob_is_unsupported();
    log2_units_round_trip();
    units_snap_to_nearest_log2_and_clamp();
    snap_level_respects_step_and_range();
    backlight_knob_needs_manual_range();
    manual_exposure_knob_is_linear_and_capped();
    knob_chain_keeps_preference_order();
    return 0;
}
