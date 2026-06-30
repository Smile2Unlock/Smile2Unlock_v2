export module su.recognizer.camera;

import std;
import su.recognizer.types;

export namespace su::recognizer {

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

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace su::recognizer
