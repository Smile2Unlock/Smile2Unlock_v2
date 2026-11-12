#include "common/log/logger.h"
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

#include <cstdio>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace su::recognizer {

namespace {

constexpr long kBacklightProperty = VideoProcAmp_BacklightCompensation;
constexpr long kExposureProperty = CameraControl_Exposure;

using HrStatus = std::expected<void, HRESULT>;

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

std::string hresult_text(const HRESULT hr) {
    char hex[11]{};
    std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(hr));
    return hex;
}

void log_control_failure(
    const std::string_view event, const std::string_view step, const HRESULT hr) {
    su::log::process_logger().warn(event, {
        su::log::field("step", step), su::log::field("hresult", hresult_text(hr))});
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
        .transform([&] { return probed; })
        .or_else([&](const HRESULT hr) -> std::expected<ProbedControl, HRESULT> {
            log_control_failure("camera.control_probe_failed", name, hr);
            return std::unexpected(hr);
        })
        .transform([](const ProbedControl& value) { return std::optional{value}; })
        .value_or(std::nullopt);
}

template <typename Interface>
void restore_control(Interface* const control, const long property,
                     const SavedControl& original, const std::string_view name) {
    if (control == nullptr) {
        return;
    }
    (void)check(control->Set(property, original.value, restore_flags(original)))
        .or_else([&](const HRESULT hr) -> HrStatus {
            log_control_failure("camera.control_restore_failed", name, hr);
            return std::unexpected(hr);
        });
}

} // namespace

class CameraExposureControls::Impl {
public:
    [[nodiscard]] detail::ExposureCapabilities probe(IMFMediaSource* const source) {
        release();
        if (source == nullptr) {
            return {};
        }
        (void)source->QueryInterface(IID_PPV_ARGS(&proc_amp_));
        (void)source->QueryInterface(IID_PPV_ARGS(&camera_control_));
        backlight_ = probe_control(proc_amp_.Get(), kBacklightProperty, "backlight");
        exposure_ = probe_control(camera_control_.Get(), kExposureProperty, "exposure");
        knob_ = detail::select_exposure_knob({},
            backlight_.and_then([](const ProbedControl& probed) {
                return detail::make_backlight_knob(probed.range, probed.original.value);
            }),
            exposure_.and_then([](const ProbedControl& probed) {
                return detail::make_manual_exposure_knob(probed.range, probed.original.value);
            }));
        log_selected_knob();
        return {.metering_region = false, .knob = knob_};
    }

    [[nodiscard]] bool set_level(const int level) {
        if (!knob_) {
            return false;
        }
        return knob_->kind == detail::ExposureKnobKind::backlight_compensation
            ? set_backlight(level) : set_manual_exposure(level);
    }

    void restore() {
        if (std::exchange(backlight_changed_, false) && backlight_) {
            restore_control(proc_amp_.Get(), kBacklightProperty, backlight_->original,
                "backlight");
        }
        if (std::exchange(exposure_changed_, false) && exposure_) {
            restore_control(camera_control_.Get(), kExposureProperty, exposure_->original,
                "exposure");
        }
    }

    void release() {
        restore();
        proc_amp_.Reset();
        camera_control_.Reset();
        backlight_.reset();
        exposure_.reset();
        knob_.reset();
    }

private:
    [[nodiscard]] static bool report(const HrStatus& status, const std::string_view step) {
        if (!status) {
            log_control_failure("camera.control_set_failed", step, status.error());
        }
        return status.has_value();
    }

    [[nodiscard]] bool set_backlight(const int level) {
        const auto value = detail::snap_level_to_range(
            level, knob_->min, knob_->max, knob_->step);
        // Mark dirty before the call so a partially applied change is still
        // undone on close.
        backlight_changed_ = true;
        return report(check(proc_amp_->Set(kBacklightProperty, value, VideoProcAmp_Flags_Manual)),
            "backlight");
    }

    [[nodiscard]] bool set_manual_exposure(const int level) {
        const auto& range = exposure_->range;
        const auto value = detail::exposure_units_to_log2(level, range.min, range.max, range.step);
        // The first call switches the camera from auto to manual exposure.
        exposure_changed_ = true;
        return report(check(camera_control_->Set(
                kExposureProperty, value, CameraControl_Flags_Manual)), "exposure");
    }

    void log_selected_knob() const {
        const auto selected = knob_ ? std::to_underlying(knob_->kind) : -1;
        su::log::process_logger().info("camera.exposure_knob", {
            su::log::field("backlight_supported", backlight_ ? 1 : 0),
            su::log::field("exposure_supported", exposure_ ? 1 : 0),
            su::log::field("selected", selected)});
    }

    ComPtr<IAMVideoProcAmp> proc_amp_;
    ComPtr<IAMCameraControl> camera_control_;
    std::optional<ProbedControl> backlight_;
    std::optional<ProbedControl> exposure_;
    std::optional<detail::ExposureKnob> knob_;
    bool backlight_changed_ = false;
    bool exposure_changed_ = false;
};

CameraExposureControls::CameraExposureControls() : impl_(std::make_unique<Impl>()) {}
CameraExposureControls::~CameraExposureControls() = default;

detail::ExposureCapabilities CameraExposureControls::probe(IMFMediaSource* const source) {
    return impl_->probe(source);
}

bool CameraExposureControls::set_level(const int level) {
    return impl_->set_level(level);
}

void CameraExposureControls::restore() {
    impl_->restore();
}

void CameraExposureControls::release() {
    impl_->release();
}

} // namespace su::recognizer
