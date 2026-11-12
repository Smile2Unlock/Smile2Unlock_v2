module;
#include "recognizer/exposure/exposure_control.h"

export module su.recognizer.camera;

import std;
import su.recognizer.types;

export namespace su::recognizer {

using detail::ExposureCapabilities;
using detail::ExposureKnobChain;
using detail::ExposureKnob;
using detail::ExposureKnobKind;
using detail::ExposureWriteError;
using detail::PixelRegion;

// A discovered V4L2 capture device.
struct CameraDevice {
    int index = 0;
    std::string name;
    std::string device_path;
};

// Enumerate available V4L2 cameras by scanning /dev/videoN.
std::vector<CameraDevice> enumerate_v4l2_cameras();

// RAII owner of a single V4L2 capture stream using mmap buffers.
class V4L2Camera {
public:
    V4L2Camera();
    ~V4L2Camera();

    V4L2Camera(const V4L2Camera&) = delete;
    V4L2Camera& operator=(const V4L2Camera&) = delete;
    V4L2Camera(V4L2Camera&&) noexcept;
    V4L2Camera& operator=(V4L2Camera&&) noexcept;

    std::expected<void, RecognizerError> open(int index);
    std::expected<CapturedFrame, RecognizerError> grab_frame();
    void release();
    [[nodiscard]] bool is_open() const;

    // Exposure control (issue #137). Capabilities are probed by open().
    // Setters report why a write failed (unsupported / transient / rejected).
    // release() (and the destructor) restore every control changed through
    // these calls to its value at open().
    [[nodiscard]] ExposureCapabilities exposure_capabilities() const;
    [[nodiscard]] std::expected<void, ExposureWriteError> set_metering_region(PixelRegion region);
    // Returns the level the camera actually applied (drivers snap requests).
    [[nodiscard]] std::expected<int, ExposureWriteError> set_exposure_level(
        ExposureKnobKind kind, int level);
    // Puts one knob back to its value at open() right away; for manual
    // exposure that hands control back to the camera's auto exposure.
    [[nodiscard]] std::expected<void, ExposureWriteError> reset_exposure_knob(
        ExposureKnobKind kind);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace su::recognizer
