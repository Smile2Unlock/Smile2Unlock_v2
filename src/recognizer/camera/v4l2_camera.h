#pragma once

#include "recognizer/image/pixel_convert.h"
#include "recognizer/recognizer_service.h"

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace su::recognizer {

// A discovered V4L2 capture device.
struct CameraDevice {
    int index = 0;
    std::string name;
    std::string device_path;
};

// Enumerate available V4L2 cameras by scanning /dev/videoN and reading each
// device's friendly name from /sys/class/video4linux. Pure I/O read, no device
// opening.
std::vector<CameraDevice> enumerate_v4l2_cameras();

// RAII owner of a single V4L2 capture stream using mmap buffers. All device
// side-effects (open, mmap, streamon/off, dqbuf) live here; frame decoding is
// a separate pure function (v4l2_frame_to_rgb).
class V4L2Camera {
public:
    V4L2Camera();
    ~V4L2Camera();

    V4L2Camera(const V4L2Camera&) = delete;
    V4L2Camera& operator=(const V4L2Camera&) = delete;
    V4L2Camera(V4L2Camera&&) noexcept;
    V4L2Camera& operator=(V4L2Camera&&) noexcept;

    // Open /dev/video<index> and negotiate a capture format. Prefers YUYV to
    // avoid per-frame JPEG decode, falls back to MJPEG.
    std::expected<void, RecognizerError> open(int index);

    // Dequeue one captured frame. The returned span points at a mmap'ed buffer
    // and is only valid until the next grab_frame call.
    std::expected<CapturedFrame, RecognizerError> grab_frame();

    // Stop streaming and release the device.
    void release();

    [[nodiscard]] bool is_open() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace su::recognizer
