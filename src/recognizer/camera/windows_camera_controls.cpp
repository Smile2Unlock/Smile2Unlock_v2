// DirectShow-style camera controls (IAMVideoProcAmp / IAMCameraControl) for
// the backlight exposure loop (issue #137). Plain TU; see the header for the
// mechanism list and the threading contract (MF worker thread only).
//
// Both interfaces are implemented by the Media Foundation capture source
// itself, so a QueryInterface on the IMFMediaSource reaches them. Cameras
// persist these settings for other applications, hence every control that was
// changed is put back to its value at probe time by restore().

#include "windows_camera_controls.h"

#include <windows.h>
#include <mfidl.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/log/logger.h"
#include "windows_hresult.h"

using Microsoft::WRL::ComPtr;

namespace su::recognizer {

namespace {

using detail::ExposureWriteError;
using HrStatus = std::expected<void, HRESULT>;
using WriteResult = std::expected<void, ExposureWriteError>;

constexpr long kBacklightProperty = VideoProcAmp_BacklightCompensation;
constexpr long kExposureProperty = CameraControl_Exposure;

[[nodiscard]] HrStatus check(const HRESULT hr) {
    return SUCCEEDED(hr) ? HrStatus{} : std::unexpected(hr);
}

// Value and auto/manual flag of one control as read before the loop touched it.
struct SavedControl {
    long value = 0;
    long flags = 0;
};

struct ProbedControl {
    detail::ProcAmpRange range;
    SavedControl original;
};

// What was attempted when a call failed, for the log line.
struct ControlOp {
    std::string_view name;
    long property = 0;
    long value = 0;
};

std::string hresult_text(const HRESULT hr) {
    char hex[11]{};
    std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(hr));
    return hex;
}

void log_control_failure(
    const std::string_view event, const ControlOp& op, const HRESULT hr) {
    su::log::process_logger().warn(event, {
        su::log::field("step", op.name),
        su::log::field("property", static_cast<int>(op.property)),
        su::log::field("value", static_cast<int>(op.value)),
        su::log::field("hresult", hresult_text(hr)),
        su::log::field("error", to_string(detail::classify_hresult(hr)))});
}

// Manual flag, even for a driver that reported no flag bits at all.
[[nodiscard]] long restore_flags(const SavedControl& saved) {
    const auto known = detail::kProcAmpCapsAuto | detail::kProcAmpCapsManual;
    return (saved.flags & known) != 0 ? saved.flags : detail::kProcAmpCapsManual;
}

// Reads range and current value. nullopt means "not supported"; the cause is
// logged because a driver that lists a control but fails the read is worth
// knowing about.
template <typename Interface>
[[nodiscard]] std::optional<ProbedControl> probe_control(
    Interface* const control, const long property, const std::string_view name) {
    if (control == nullptr) {
        return std::nullopt;
    }
    auto probed = ProbedControl{};
    return check(control->GetRange(property, &probed.range.min, &probed.range.max,
                     &probed.range.step, &probed.range.default_value, &probed.range.caps_flags))
        .and_then([&] {
            return check(control->Get(property, &probed.original.value, &probed.original.flags));
        })
        .transform([&] { return std::optional{probed}; })
        .or_else([&](const HRESULT hr) -> std::expected<std::optional<ProbedControl>, HRESULT> {
            log_control_failure("camera.control_probe_failed", {name, property, 0}, hr);
            return std::nullopt;
        })
        .value_or(std::nullopt);
}

// One Set call with the failure logged and mapped to the loop's error type.
template <typename Interface>
[[nodiscard]] WriteResult write_control(Interface* const control, const std::string_view event,
    const ControlOp& op, const long flags) {
    if (control == nullptr) {
        return std::unexpected(ExposureWriteError::unsupported);
    }
    return check(control->Set(op.property, op.value, flags))
        .transform_error([&](const HRESULT hr) {
            log_control_failure(event, op, hr);
            return detail::classify_hresult(hr);
        });
}

// A control plus the bookkeeping to undo what the loop did to it.
template <typename Interface>
struct Tracked {
    ComPtr<Interface> control;
    long property = 0;
    std::string_view name;
    std::optional<ProbedControl> probed;
    std::optional<detail::ExposureKnob> knob;
    // Set before the first write so a partially applied change is undone too;
    // cleared only after a successful restore.
    bool changed = false;

    void attach(Interface* const raw, std::optional<ProbedControl> read) {
        control = raw;
        probed = std::move(read);
    }

    void clear() {
        control.Reset();
        probed.reset();
        knob.reset();
        changed = false;
    }

    [[nodiscard]] WriteResult write(const long value, const long flags) {
        changed = true;
        return write_control(control.Get(), "camera.control_set_failed",
            {name, property, value}, flags);
    }

    [[nodiscard]] WriteResult restore() {
        if (!probed || !control) {
            return std::unexpected(ExposureWriteError::unsupported);
        }
        if (!changed) {
            return {};
        }
        const auto& original = probed->original;
        return write_control(control.Get(), "camera.control_restore_failed",
                   {name, property, original.value}, restore_flags(original))
            .transform([this] { changed = false; });
    }
};

} // namespace

class CameraExposureControls::Impl {
public:
    Impl() {
        backlight_.property = kBacklightProperty;
        backlight_.name = "backlight";
        exposure_.property = kExposureProperty;
        exposure_.name = "exposure";
    }

    [[nodiscard]] detail::ExposureCapabilities probe(IMFMediaSource* const source) {
        ComPtr<IAMVideoProcAmp> proc_amp;
        ComPtr<IAMCameraControl> camera_control;
        if (source != nullptr) {
            (void)source->QueryInterface(IID_PPV_ARGS(&proc_amp));
            (void)source->QueryInterface(IID_PPV_ARGS(&camera_control));
        }
        return probe(proc_amp.Get(), camera_control.Get());
    }

    [[nodiscard]] detail::ExposureCapabilities probe(
        IAMVideoProcAmp* const proc_amp, IAMCameraControl* const camera_control) {
        release();
        backlight_.attach(proc_amp, probe_control(proc_amp, kBacklightProperty, "backlight"));
        exposure_.attach(
            camera_control, probe_control(camera_control, kExposureProperty, "exposure"));
        backlight_.knob = backlight_.probed.and_then([](const ProbedControl& probed) {
            return detail::make_backlight_knob(probed.range, probed.original.value);
        });
        exposure_.knob = exposure_.probed.and_then([](const ProbedControl& probed) {
            return detail::make_manual_exposure_knob(probed.range, probed.original.value);
        });
        const auto chain = detail::make_knob_chain({}, backlight_.knob, exposure_.knob);
        log_knob_chain(chain);
        return {.metering_region = false, .knobs = chain};
    }

    [[nodiscard]] std::expected<int, ExposureWriteError> set_level(
        const detail::ExposureKnobKind kind, const int level) {
        switch (kind) {
        case detail::ExposureKnobKind::backlight_compensation:
            return backlight_.knob ? set_backlight(level)
                                   : std::unexpected(ExposureWriteError::unsupported);
        case detail::ExposureKnobKind::manual_exposure:
            return exposure_.knob ? set_manual_exposure(level)
                                  : std::unexpected(ExposureWriteError::unsupported);
        case detail::ExposureKnobKind::ev_bias:
            return std::unexpected(ExposureWriteError::unsupported);  // no verified mechanism
        }
        std::unreachable();
    }

    [[nodiscard]] WriteResult reset(const detail::ExposureKnobKind kind) {
        switch (kind) {
        case detail::ExposureKnobKind::backlight_compensation:
            return backlight_.restore();
        case detail::ExposureKnobKind::manual_exposure:
            return exposure_.restore();
        case detail::ExposureKnobKind::ev_bias:
            return std::unexpected(ExposureWriteError::unsupported);
        }
        std::unreachable();
    }

    // Reverse of the escalation order, like the V4L2 backend. Both restores
    // always run: a failing one must not leave the other control changed.
    [[nodiscard]] bool restore() {
        const auto exposure = exposure_.restore();
        const auto backlight = backlight_.restore();
        return !pending(exposure) && !pending(backlight);
    }

    void release() {
        for (auto attempt = 0; attempt < kRestoreAttempts && !restore(); ++attempt) {
        }
        backlight_.clear();
        exposure_.clear();
    }

private:
    // A control that is merely not present has nothing left to restore.
    [[nodiscard]] static bool pending(const WriteResult& result) {
        return !result && result.error() != ExposureWriteError::unsupported;
    }

    [[nodiscard]] std::expected<int, ExposureWriteError> set_backlight(const int level) {
        const auto& knob = *backlight_.knob;
        const auto value = static_cast<int>(
            detail::snap_level_to_range(level, knob.min, knob.max, knob.step));
        return backlight_.write(value, VideoProcAmp_Flags_Manual).transform([value] {
            return value;
        });
    }

    // The first call switches the camera from auto to manual exposure.
    [[nodiscard]] std::expected<int, ExposureWriteError> set_manual_exposure(const int level) {
        const auto& range = exposure_.probed->range;
        const auto snapped = detail::snap_exposure(level, range.min, range.max, range.step);
        return exposure_.write(snapped.log2_seconds, CameraControl_Flags_Manual)
            .transform([snapped] { return snapped.units; });
    }

    static void log_knob_chain(const detail::ExposureKnobChain& chain) {
        su::log::process_logger().info("camera.exposure_knob", {
            su::log::field("backlight_supported",
                chain.end() != std::ranges::find(chain, detail::ExposureKnobKind::backlight_compensation,
                    &detail::ExposureKnob::kind) ? 1 : 0),
            su::log::field("manual_supported",
                chain.end() != std::ranges::find(chain, detail::ExposureKnobKind::manual_exposure,
                    &detail::ExposureKnob::kind) ? 1 : 0),
            su::log::field("chain_length", static_cast<int>(chain.size()))});
    }

    Tracked<IAMVideoProcAmp> backlight_;
    Tracked<IAMCameraControl> exposure_;
};

CameraExposureControls::CameraExposureControls() : impl_(std::make_unique<Impl>()) {}
CameraExposureControls::~CameraExposureControls() = default;

detail::ExposureCapabilities CameraExposureControls::probe(IMFMediaSource* const source) {
    return impl_->probe(source);
}

detail::ExposureCapabilities CameraExposureControls::probe(
    IAMVideoProcAmp* const proc_amp, IAMCameraControl* const camera_control) {
    return impl_->probe(proc_amp, camera_control);
}

std::expected<int, detail::ExposureWriteError> CameraExposureControls::set_level(
    const detail::ExposureKnobKind kind, const int level) {
    return impl_->set_level(kind, level);
}

std::expected<void, detail::ExposureWriteError> CameraExposureControls::reset(
    const detail::ExposureKnobKind kind) {
    return impl_->reset(kind);
}

bool CameraExposureControls::restore() {
    return impl_->restore();
}

void CameraExposureControls::release() {
    impl_->release();
}

} // namespace su::recognizer
