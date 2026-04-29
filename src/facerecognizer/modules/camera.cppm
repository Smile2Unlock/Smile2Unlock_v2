module;

#include <seeta/Common/CStruct.h>

export module su.facerecognizer.camera;

import std;

export namespace su::facerecognizer {

class ICaptureDevice {
public:
    virtual ~ICaptureDevice() = default;
    virtual bool is_initialized() const = 0;
    virtual bool capture_frame(SeetaImageData& image) = 0;
};

class CameraCapture final : public ICaptureDevice {
public:
    explicit CameraCapture(int camera_index = 0) : camera_index_(camera_index) {}
    ~CameraCapture() override = default;

    bool is_initialized() const override {
        return camera_index_ >= 0;
    }

    bool capture_frame(SeetaImageData& image) override {
        image.width = 0;
        image.height = 0;
        image.channels = 0;
        image.data = nullptr;
        return false;
    }

    static std::vector<std::string> enumerate_cameras() {
        return {"camera#0"};
    }

private:
    int camera_index_ = 0;
};

}  // namespace su::facerecognizer
