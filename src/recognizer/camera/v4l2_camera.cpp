#include "recognizer/camera/v4l2_camera.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/videodev2.h>
#include <memory>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace su::recognizer {

namespace {

constexpr int kMmapBufferCount = 4;

// Read the friendly name recorded in /sys/class/video4linux/videoN/name.
std::string read_camera_name(int index) {
    const auto path = std::filesystem::path("/sys/class/video4linux") /
                      ("video" + std::to_string(index)) / "name";
    auto input = std::ifstream(path);
    if (!input.is_open()) {
        return "/dev/video" + std::to_string(index);
    }
    std::string name;
    std::getline(input, name);
    if (name.empty()) {
        return "/dev/video" + std::to_string(index);
    }
    return name;
}

bool device_supports_capture(int fd) {
    v4l2_capability cap{};
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        return false;
    }
    return (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0;
}

// Try to set the requested format; returns the format actually negotiated.
std::expected<uint32_t, RecognizerError> negotiate_format(int fd, int width, int height) {
    for (const auto preferred : {V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_MJPEG}) {
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width;
        fmt.fmt.pix.height = height;
        fmt.fmt.pix.pixelformat = preferred;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            continue;
        }
        if (fmt.fmt.pix.pixelformat == preferred) {
            return preferred;
        }
    }
    return std::unexpected(RecognizerError::kCameraUnavailable);
}

}  // namespace

std::vector<CameraDevice> enumerate_v4l2_cameras() {
    auto devices = std::vector<CameraDevice>{};
    for (int index = 0; index < 64; ++index) {
        const auto path = "/dev/video" + std::to_string(index);
        if (!std::filesystem::exists(path)) {
            continue;
        }
        devices.push_back(CameraDevice{
            .index = index,
            .name = read_camera_name(index),
            .device_path = path,
        });
    }
    return devices;
}

class V4L2Camera::Impl {
public:
    Impl() = default;
    ~Impl() { release(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    std::expected<void, RecognizerError> open(int index) {
        device_path_ = "/dev/video" + std::to_string(index);
        fd_ = ::open(device_path_.c_str(), O_RDWR);
        if (fd_ < 0) {
            return std::unexpected(RecognizerError::kNoCamera);
        }
        if (!device_supports_capture(fd_)) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }

        auto format = negotiate_format(fd_, 640, 480);
        if (!format) {
            return std::unexpected(format.error());
        }
        negotiated_format_ = *format;

        v4l2_requestbuffers req{};
        req.count = kMmapBufferCount;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }

        buffers_.resize(req.count);
        for (std::size_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                return std::unexpected(RecognizerError::kCameraUnavailable);
            }
            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd_, buf.m.offset);
            if (buffers_[i].start == MAP_FAILED) {
                return std::unexpected(RecognizerError::kCameraUnavailable);
            }
        }

        for (std::size_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
                return std::unexpected(RecognizerError::kCameraUnavailable);
            }
        }

        const auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }
        streaming_ = true;
        return {};
    }

    std::expected<CapturedFrame, RecognizerError> grab_frame() {
        if (fd_ < 0 || !streaming_) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }

        const auto& buffer = buffers_[buf.index];
        CapturedFrame frame{
            .width = 640,
            .height = 480,
            .v4l2_format = negotiated_format_,
            .bytes = std::span<const std::byte>(
                static_cast<const std::byte*>(buffer.start), buf.bytesused),
        };

        // Requeue the buffer for the next capture; remember the index so the
        // caller's span stays valid until the next grab.
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }
        return frame;
    }

    void release() {
        if (streaming_) {
            const auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for (auto& buffer : buffers_) {
            if (buffer.start != nullptr && buffer.start != MAP_FAILED) {
                munmap(buffer.start, buffer.length);
            }
        }
        buffers_.clear();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    [[nodiscard]] bool is_open() const { return fd_ >= 0 && streaming_; }

private:
    struct MmapBuffer {
        void* start = nullptr;
        std::size_t length = 0;
    };

    std::string device_path_;
    int fd_ = -1;
    uint32_t negotiated_format_ = 0;
    bool streaming_ = false;
    std::vector<MmapBuffer> buffers_{};
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
    return impl_ && impl_->is_open();
}

}  // namespace su::recognizer
