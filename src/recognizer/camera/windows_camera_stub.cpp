module;
#include <cstdint>

module su.recognizer.camera;

namespace su::recognizer {

// Windows stub: no V4L2 capture backend yet. Camera enumeration returns
// nothing and every capture operation reports the camera as unavailable.
// A Media Foundation backend can replace this implementation unit later
// without touching the module interface or recognizer service.

std::vector<CameraDevice> enumerate_v4l2_cameras() {
    return {};
}

class V4L2Camera::Impl {
public:
    std::expected<void, RecognizerError> open(int) {
        return std::unexpected(RecognizerError::kCameraUnavailable);
    }

    std::expected<CapturedFrame, RecognizerError> grab_frame() {
        return std::unexpected(RecognizerError::kCameraUnavailable);
    }

    void release() {}
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
    return impl_ != nullptr;
}

} // namespace su::recognizer
