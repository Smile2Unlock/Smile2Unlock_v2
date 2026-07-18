module;
#include <cstdint>
#include <cerrno>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>

module su.recognizer.camera;

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
    const auto capabilities = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
        ? cap.device_caps
        : cap.capabilities;
    return (capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0;
}

struct NegotiatedFormat {
    uint32_t pixel_format = 0;
    int width = 0;
    int height = 0;
    std::size_t stride = 0;
};

// V4L2 may adjust dimensions and row stride even when it accepts the requested
// pixel format. Preserve the complete negotiated shape for downstream bounds
// checks and conversion.
std::expected<NegotiatedFormat, RecognizerError> negotiate_format(int fd, int width, int height) {
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
            const auto fallback_stride = preferred == V4L2_PIX_FMT_YUYV
                ? static_cast<std::size_t>(fmt.fmt.pix.width) * 2
                : 0;
            return NegotiatedFormat{
                .pixel_format = preferred,
                .width = static_cast<int>(fmt.fmt.pix.width),
                .height = static_cast<int>(fmt.fmt.pix.height),
                .stride = fmt.fmt.pix.bytesperline != 0
                    ? fmt.fmt.pix.bytesperline
                    : fallback_stride,
            };
        }
    }
    return std::unexpected(RecognizerError::kCameraUnavailable);
}

}  // namespace

std::vector<CameraDevice> enumerate_v4l2_cameras() {
    auto range = std::views::iota(0, 64)
        | std::views::filter([](int index) {
              return std::filesystem::exists("/dev/video" + std::to_string(index));
          })
        | std::views::transform([](int index) {
              return CameraDevice{
                  .index = index,
                  .name = read_camera_name(index),
                  .device_path = "/dev/video" + std::to_string(index),
              };
          });
    return std::vector<CameraDevice>(range.begin(), range.end());
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
        release();
        device_path_ = "/dev/video" + std::to_string(index);
        fd_ = ::open(device_path_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            return std::unexpected(RecognizerError::kNoCamera);
        }
        struct OpenRollback {
            Impl* owner;
            ~OpenRollback() {
                if (owner != nullptr) {
                    owner->release();
                }
            }
            void dismiss() { owner = nullptr; }
        } rollback{this};
        if (!device_supports_capture(fd_)) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }

        auto format = negotiate_format(fd_, 640, 480);
        if (!format) {
            return std::unexpected(format.error());
        }
        negotiated_format_ = format->pixel_format;
        width_ = format->width;
        height_ = format->height;
        stride_ = format->stride;

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
        rollback.dismiss();
        return {};
    }

    std::expected<CapturedFrame, RecognizerError> grab_frame() {
        if (fd_ < 0 || !streaming_) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }

        // Requeue the buffer held from the previous grab so the driver can
        // refill it. By deferring the QBUF to the next call (instead of doing
        // it immediately after DQBUF), the span returned last time stays valid
        // for the whole interval between grabs, so the caller's pixel copy is
        // not racing with the capture hardware overwriting the same mmap'd
        // buffer. This is what fixes the intermittent black/tearing frames.
        if (held_buffer_index_) {
            v4l2_buffer requeue{};
            requeue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            requeue.memory = V4L2_MEMORY_MMAP;
            requeue.index = *held_buffer_index_;
            if (ioctl(fd_, VIDIOC_QBUF, &requeue) < 0) {
                held_buffer_index_.reset();
                return std::unexpected(RecognizerError::kCameraUnavailable);
            }
            held_buffer_index_.reset();
        }

        auto descriptor = pollfd{
            .fd = fd_,
            .events = POLLIN,
            .revents = 0,
        };
        int poll_result = 0;
        do {
            poll_result = ::poll(&descriptor, 1, 500);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }

        if (buf.index >= buffers_.size()) {
            return std::unexpected(RecognizerError::kCameraUnavailable);
        }
        const auto& buffer = buffers_[buf.index];
        if (buf.bytesused > buffer.length) {
            return std::unexpected(RecognizerError::kInvalidImage);
        }
        held_buffer_index_ = buf.index;
        return CapturedFrame{
            .width = width_,
            .height = height_,
            .stride = stride_,
            .v4l2_format = negotiated_format_,
            .bytes = std::span<const std::byte>(
                static_cast<const std::byte*>(buffer.start), buf.bytesused),
        };
    }

    void release() {
        if (streaming_) {
            const auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        held_buffer_index_.reset();
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
        negotiated_format_ = 0;
        width_ = 0;
        height_ = 0;
        stride_ = 0;
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
    int width_ = 0;
    int height_ = 0;
    std::size_t stride_ = 0;
    bool streaming_ = false;
    std::vector<MmapBuffer> buffers_{};
    // Index of the buffer currently held out (DQBUF'd but not yet requeued).
    // The returned span points into this buffer and stays valid until the next
    // grab_frame call requeues it. Requeuing only on the next grab gives the
    // caller a full frame interval to copy the data before the driver reuses
    // the buffer, instead of requeuing immediately inside grab_frame and
    // letting the next capture overwrite the bytes the caller just received.
    std::optional<uint32_t> held_buffer_index_{};
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
