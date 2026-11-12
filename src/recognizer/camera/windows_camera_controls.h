// Windows camera exposure controls for the backlight loop (issue #137).
//
// Plain header (no windows.h): the constexpr range/unit helpers are shared
// with the unit tests; the COM-facing CameraExposureControls class is
// implemented in windows_camera_controls.cpp and must only be used from the MF
// worker thread.
//
// Knobs implemented here, in escalation order:
//   IAMVideoProcAmp  VideoProcAmp_BacklightCompensation  -> backlight_compensation
//   IAMCameraControl CameraControl_Exposure (manual)     -> manual_exposure
// The ROI and EV compensation of IMFExtendedCameraController are NOT
// implemented: mingw-w64 lacks the declarations and the IIDs / vtable order /
// payload layouts could not be verified against an authoritative source, so
// the chain never contains ev_bias and there is no metering region.
#pragma once

#include <algorithm>
#include <memory>
#include <optional>

#include "recognizer/exposure/exposure_control.h"

struct IMFMediaSource;

namespace su::recognizer::detail {

// Loop-facing manual exposure unit: 100 microseconds (the loop scales it
// multiplicatively, so it must be linear; DirectShow is log2 seconds).
inline constexpr int kExposureUnitsPerSecond = 10000;
// Never expose longer than 1/30 s: motion blur and a frame rate collapse
// would hurt recognition more than a dark face.
inline constexpr int kMaxManualExposureUnits = kExposureUnitsPerSecond / 30;
inline constexpr int kMinExposureUnits = 1;
inline constexpr int kMaxExposureUnits = 1'000'000'000;

// Exposure exponents outside this window are meaningless for a camera
// (2^-40 s .. 2^20 s) and keep the loops below bounded.
inline constexpr int kMinLog2Seconds = -40;
inline constexpr int kMaxLog2Seconds = 20;

// CapsFlags bits from IAMVideoProcAmp/IAMCameraControl::GetRange
// (VideoProcAmp_Flags_* / CameraControl_Flags_* in strmif.h: Auto=1, Manual=2).
inline constexpr long kProcAmpCapsAuto = 0x1;
inline constexpr long kProcAmpCapsManual = 0x2;

// Result of IAMVideoProcAmp/IAMCameraControl::GetRange.
struct ProcAmpRange {
    long min = 0;
    long max = 0;
    long step = 1;
    long default_value = 0;
    long caps_flags = 0;
};

// 2^exponent without <cmath>: std::exp2 is not constexpr, and exposure
// exponents are small integers, so repeated doubling is exact.
[[nodiscard]] constexpr double pow2(const int exponent) {
    auto result = 1.0;
    for (auto i = 0; i < (exponent < 0 ? -exponent : exponent); ++i) {
        result *= 2.0;
    }
    return exponent < 0 ? 1.0 / result : result;
}

[[nodiscard]] constexpr int exposure_log2_to_units(const long log2_seconds) {
    const auto exponent = std::clamp<long>(log2_seconds, kMinLog2Seconds, kMaxLog2Seconds);
    const auto units = static_cast<long long>(
        pow2(static_cast<int>(exponent)) * kExposureUnitsPerSecond + 0.5);
    return static_cast<int>(std::clamp<long long>(units, kMinExposureUnits, kMaxExposureUnits));
}

// Rounds `value` onto min + k*step, clamped to [min, max] (step <= 0 means 1).
[[nodiscard]] constexpr long snap_level_to_range(
    const long value, const long min, const long max, const long step) {
    const auto stride = std::max<long>(step, 1);
    const auto clamped = std::clamp(value, min, std::max(min, max));
    const auto snapped = min + (clamped - min + stride / 2) / stride * stride;
    return snapped > max ? std::max(snapped - stride, min) : snapped;
}

// Nearest DirectShow log2-seconds value (on the driver's step grid) for a
// linear exposure in loop units. "Nearest" is in the log domain: the boundary
// between 2^v and 2^(v+1) is their geometric mean, compared squared so no
// logarithm (not constexpr) is needed.
[[nodiscard]] constexpr long exposure_units_to_log2(
    const int units, const long min_log2, const long max_log2, const long step_log2) {
    const auto target = static_cast<double>(std::max(units, kMinExposureUnits));
    auto exponent = kMinLog2Seconds;
    while (exponent < kMaxLog2Seconds) {
        const auto lower = kExposureUnitsPerSecond * pow2(exponent);
        if (target * target <= lower * lower * 2.0) {
            break;
        }
        ++exponent;
    }
    return snap_level_to_range(exponent, min_log2, max_log2, step_log2);
}

[[nodiscard]] constexpr std::optional<ExposureKnob> make_backlight_knob(
    const ProcAmpRange& range, const long current) {
    if (range.max <= range.min || (range.caps_flags & kProcAmpCapsManual) == 0) {
        return std::nullopt;
    }
    return ExposureKnob{
        .kind = ExposureKnobKind::backlight_compensation,
        .min = static_cast<int>(range.min),
        .max = static_cast<int>(range.max),
        .step = static_cast<int>(std::max<long>(range.step, 1)),
        .initial = static_cast<int>(
            snap_level_to_range(current, range.min, range.max, range.step)),
    };
}

// Range and initial value are reported in linear loop units; the cap keeps
// only exposures up to 1/30 s.
[[nodiscard]] constexpr std::optional<ExposureKnob> make_manual_exposure_knob(
    const ProcAmpRange& range, const long current_log2) {
    if ((range.caps_flags & kProcAmpCapsManual) == 0 || range.max < range.min) {
        return std::nullopt;
    }
    const auto min_units = exposure_log2_to_units(range.min);
    const auto max_units = std::min(exposure_log2_to_units(range.max), kMaxManualExposureUnits);
    if (max_units <= min_units) {
        return std::nullopt;
    }
    return ExposureKnob{
        .kind = ExposureKnobKind::manual_exposure,
        .min = min_units,
        .max = max_units,
        .step = 1,
        .initial = std::clamp(exposure_log2_to_units(current_log2), min_units, max_units),
    };
}

// Compile-time checks of the conversions the loop relies on.
static_assert(exposure_log2_to_units(-5) == 313);  // 1/32 s = 312.5 * 100 us
static_assert(exposure_log2_to_units(0) == kExposureUnitsPerSecond);
static_assert(exposure_log2_to_units(-30) == kMinExposureUnits);
static_assert(exposure_units_to_log2(200, -11, -2, 1) == -6);  // 1/50 s is nearer 1/64
static_assert(exposure_units_to_log2(250, -11, -2, 1) == -5);
static_assert(exposure_units_to_log2(1, -8, -2, 1) == -8);
static_assert(snap_level_to_range(99, 0, 10, 4) == 8);
static_assert(make_manual_exposure_knob({-11, -2, 1, -5, kProcAmpCapsManual}, -6)->max
    == kMaxManualExposureUnits);

}  // namespace su::recognizer::detail

namespace su::recognizer {

// Owns the DirectShow control interfaces of one camera source. Every method
// must run on the MF worker thread that owns the source.
class CameraExposureControls {
public:
    CameraExposureControls();
    ~CameraExposureControls();
    CameraExposureControls(const CameraExposureControls&) = delete("owns COM interfaces");
    CameraExposureControls& operator=(const CameraExposureControls&) =
        delete("owns COM interfaces");

    // Reads ranges and current values. Never fails: problems are logged and
    // leave the capabilities empty.
    [[nodiscard]] detail::ExposureCapabilities probe(IMFMediaSource* source);
    // Applies a loop level to one knob; false when unsupported or rejected by
    // the driver. The first manual exposure write switches auto -> manual.
    [[nodiscard]] bool set_level(detail::ExposureKnobKind kind, int level);
    // Puts one knob back to its value at probe() (manual exposure returns to
    // its original auto/manual mode). True when nothing needed undoing.
    [[nodiscard]] bool reset(detail::ExposureKnobKind kind);
    // Puts every control changed since probe() back to its value at probe().
    void restore();
    // restore() plus dropping the interfaces; call before the source shuts down.
    void release();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace su::recognizer
