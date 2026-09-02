// Windows Media Foundation camera backend (plain header, no windows.h).
//
// Kept as a plain (non-module) translation unit pair because winnt.h wraps
// <x86intrin.h> in `extern "C"`, which conflicts with the declarations
// imported via `import std;` when compiled as a module unit (GCC 16 mingw).
// The su.recognizer.camera module unit wraps these functions/classes and
// converts to the V4L2-shaped CameraDevice/CapturedFrame types.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace su::recognizer {

// A discovered video-capture device (index matches the camera picker order).
struct MfCameraDeviceInfo {
    int index = 0;
    std::string name;
    std::string symbolic_link;
};

// Enumerate video-capture devices via MFEnumDeviceSources. Safe to call
// without prior COM/MF initialization; the backend self-initializes.
std::vector<MfCameraDeviceInfo> mf_enumerate_cameras();

// One captured frame. `v4l2_format` carries the same values the V4L2 camera
// backend uses (0x56595559 'YUYV' / 0x47504A4D 'MJPG'); the MF backend
// reports MF YUY2 frames as YUYV (identical packing) and MJPG as MJPEG.
// `bytes` is owned by the stream and valid until the next grab call.
struct MfCameraFrame {
    int width = 0;
    int height = 0;
    std::size_t stride = 0;
    std::uint32_t v4l2_format = 0;
    const std::uint8_t* bytes = nullptr;
    std::size_t byte_count = 0;
};

// RAII owner of a Media Foundation source reader. All MF calls run on a
// private MTA worker thread so the caller's apartment state never matters.
class MfCameraStream {
public:
    MfCameraStream();
    ~MfCameraStream();

    MfCameraStream(const MfCameraStream&) = delete;
    MfCameraStream& operator=(const MfCameraStream&) = delete;
    MfCameraStream(MfCameraStream&&) = delete;
    MfCameraStream& operator=(MfCameraStream&&) = delete;

    bool open(int index);
    bool grab(MfCameraFrame& out);
    void close();
    [[nodiscard]] bool is_open() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace su::recognizer
