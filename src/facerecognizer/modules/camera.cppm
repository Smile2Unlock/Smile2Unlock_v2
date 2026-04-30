module;

#include <expected>
#include <seeta/Common/CStruct.h>

export module su.facerecognizer.camera;

import std;

export namespace su::facerecognizer {

enum class CaptureError { kDeviceUnavailable, kReadFailed, kTimeout };

class ICaptureDevice {
public:
    virtual ~ICaptureDevice() = default;
    virtual bool is_initialized() const = 0;
    virtual std::expected<SeetaImageData, CaptureError> capture_frame() = 0;
};

class CameraCapture final : public ICaptureDevice {
public:
    explicit CameraCapture(int camera_index = 0) : camera_index_(camera_index) {}
    ~CameraCapture() override = default;

    bool is_initialized() const override {
        return camera_index_ >= 0;
    }

    std::expected<SeetaImageData, CaptureError> capture_frame() override {
        return std::unexpected(CaptureError::kDeviceUnavailable);
    }

    static std::vector<std::string> enumerate_cameras() {
        return {"camera#0"};
    }

private:
    int camera_index_ = 0;
};

}  // namespace su::facerecognizer
