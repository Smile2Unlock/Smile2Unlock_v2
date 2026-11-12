// Windows Media Foundation camera backend (module unit).
//
// The heavy lifting lives in the plain TU windows_mf_camera.cpp (MF headers
// cannot be included in a module unit that imports std; see the header for
// details). This unit adapts the MF-shaped results to the V4L2-shaped
// CameraDevice / CapturedFrame contract of su.recognizer.camera.

module;
#include <cstddef>
#include <vector>

#include "windows_mf_camera.h"

module su.recognizer.camera;

namespace su::recognizer {

std::vector<CameraDevice> enumerate_v4l2_cameras() {
    const auto devices = mf_enumerate_cameras();
    auto cameras = std::vector<CameraDevice>{};
    cameras.reserve(devices.size());
    for (const auto& device : devices) {
        cameras.push_back(CameraDevice{
            .index = device.index,
            .name = device.name,
            .device_path = device.symbolic_link,
        });
    }
    return cameras;
}

class V4L2Camera::Impl {
public:
    std::expected<void, RecognizerError> open(int index) {
        if (!stream_.open(index)) {
            return std::unexpected(stream_.last_open_was_permission_or_busy()
                ? RecognizerError::kCameraPermissionOrBusy
                : RecognizerError::kCameraUnavailable);
        }
        return {};
    }

    std::expected<CapturedFrame, RecognizerError> grab_frame() {
        MfCameraFrame frame;
        if (!stream_.grab(frame) || frame.bytes == nullptr) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }
        // Copy into a stream-owned buffer so the span stays valid until the
        // next grab (mirrors the V4L2 mmap contract).
        buffer_.assign(
            reinterpret_cast<const std::byte*>(frame.bytes),
            reinterpret_cast<const std::byte*>(frame.bytes + frame.byte_count));
        return CapturedFrame{
            .width = frame.width,
            .height = frame.height,
            .stride = frame.stride,
            .v4l2_format = frame.v4l2_format,
            .bytes = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(buffer_.data()),
                buffer_.size()),
        };
    }

    void release() {
        stream_.close();
    }

    bool is_open() const {
        return stream_.is_open();
    }

    ExposureCapabilities exposure_capabilities() const {
        return stream_.exposure_capabilities();
    }

    // The MF stream still reports success as a bool; a failed HRESULT maps to
    // rejected and the requested level is assumed applied. Refining both
    // (transient errors, the driver's snapped level) is Windows follow-up work.
    std::expected<void, ExposureWriteError> set_metering_region(const PixelRegion& region) {
        if (stream_.set_metering_region(region)) {
            return {};
        }
        return std::unexpected(ExposureWriteError::unsupported);
    }

    std::expected<int, ExposureWriteError> set_exposure_level(ExposureKnobKind kind, int level) {
        if (stream_.set_exposure_level(kind, level)) {
            return level;
        }
        return std::unexpected(ExposureWriteError::rejected);
    }

    std::expected<void, ExposureWriteError> reset_exposure_knob(ExposureKnobKind kind) {
        if (stream_.reset_exposure_knob(kind)) {
            return {};
        }
        return std::unexpected(ExposureWriteError::rejected);
    }

private:
    MfCameraStream stream_;
    std::vector<std::byte> buffer_;
};

V4L2Camera::V4L2Camera() = default;

V4L2Camera::~V4L2Camera() = default;

V4L2Camera::V4L2Camera(V4L2Camera&&) noexcept = default;

V4L2Camera& V4L2Camera::operator=(V4L2Camera&&) noexcept = default;

std::expected<void, RecognizerError> V4L2Camera::open(int index) {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    return impl_->open(index);
}

std::expected<CapturedFrame, RecognizerError> V4L2Camera::grab_frame() {
    if (!impl_) {
        return std::unexpected(RecognizerError::kCameraUnavailable);
    }
    return impl_->grab_frame();
}

void V4L2Camera::release() {
    if (impl_) {
        impl_->release();
    }
}

bool V4L2Camera::is_open() const {
    return impl_ != nullptr && impl_->is_open();
}

ExposureCapabilities V4L2Camera::exposure_capabilities() const {
    return impl_ ? impl_->exposure_capabilities() : ExposureCapabilities{};
}

std::expected<void, ExposureWriteError> V4L2Camera::set_metering_region(PixelRegion region) {
    if (!impl_) {
        return std::unexpected(ExposureWriteError::unsupported);
    }
    return impl_->set_metering_region(region);
}

std::expected<int, ExposureWriteError> V4L2Camera::set_exposure_level(
    ExposureKnobKind kind, int level) {
    if (!impl_) {
        return std::unexpected(ExposureWriteError::unsupported);
    }
    return impl_->set_exposure_level(kind, level);
}

std::expected<void, ExposureWriteError> V4L2Camera::reset_exposure_knob(ExposureKnobKind kind) {
    if (!impl_) {
        return std::unexpected(ExposureWriteError::unsupported);
    }
    return impl_->reset_exposure_knob(kind);
}

} // namespace su::recognizer
