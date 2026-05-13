module;

#include <libyuv.h>
#include <seeta/Common/CStruct.h>

#include <expected>

#ifdef __linux__
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/utils/result.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>
#endif

export module su.facerecognizer.camera;

import std;

// ============================================================================
// Exported types
// ============================================================================

export namespace su::facerecognizer {

enum class CaptureError : std::uint8_t { kDeviceUnavailable, kReadFailed, kTimeout };

class ICaptureDevice {
   public:
    ICaptureDevice() = default;
    virtual ~ICaptureDevice() = default;

    ICaptureDevice(const ICaptureDevice&) = default;
    auto operator=(const ICaptureDevice&) -> ICaptureDevice& = default;
    ICaptureDevice(ICaptureDevice&&) = default;
    auto operator=(ICaptureDevice&&) -> ICaptureDevice& = default;

    [[nodiscard]] virtual auto is_initialized() const -> bool = 0;
    [[nodiscard]] virtual auto capture_frame() -> std::expected<SeetaImageData, CaptureError> = 0;
};

}  // namespace su::facerecognizer

// ============================================================================
// Internal helpers and backends
// ============================================================================

namespace su::facerecognizer {

namespace {

[[nodiscard]] auto build_frame(const void* data, int width, uint32_t fourcc, int height)
    -> std::optional<SeetaImageData>
{
    const auto frame_size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U;
    const auto argb_size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;

    // RAII-managed allocation; released into SeetaImageData only on success.
    auto rgb = std::unique_ptr<unsigned char, void (*)(unsigned char*)>(
        new unsigned char[frame_size],
        [](unsigned char* p) { delete[] p; });  // NOLINT: delete[] needs non-const

    auto argb = std::vector<uint8_t>(argb_size);
    const int argb_stride = width * 4;

    // Build a yuv-to-argb converter for the given fourcc.
    const auto convert_yuv = [=](uint8_t* dst, int stride) -> int {
        const auto* src = static_cast<const uint8_t*>(data);
        switch (fourcc) {
            case V4L2_PIX_FMT_YUYV:
                return libyuv::YUY2ToARGB(src, width * 2, dst, stride, width, height);
            case V4L2_PIX_FMT_NV12: {
                const auto uv_offset = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
                return libyuv::NV12ToARGB(src, width, src + uv_offset, width, dst, stride, width, height);
            }
            default:
                return -1;
        }
    };

    if (convert_yuv(argb.data(), argb_stride) != 0) {
        return std::nullopt;
    }

    if (libyuv::ARGBToRAW(argb.data(), argb_stride, rgb.get(), width * 3, width, height) != 0) {
        return std::nullopt;
    }

    return SeetaImageData{
        .width = width,
        .height = height,
        .channels = 3,
        .data = rgb.release(),
    };
}

[[nodiscard]] auto supports_format(int dev_fd, uint32_t pix_fmt) -> bool {
    struct v4l2_fmtdesc desc = {};
    desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (desc.index = 0; ::ioctl(dev_fd, VIDIOC_ENUM_FMT, &desc) == 0; ++desc.index) {
        if (desc.pixelformat == pix_fmt) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto try_format(int dev_fd, uint32_t width, uint32_t height, uint32_t pix_fmt)
    -> std::optional<v4l2_format>
{
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = pix_fmt;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (::ioctl(dev_fd, VIDIOC_S_FMT, &fmt) == 0 && fmt.fmt.pix.pixelformat != 0) {
        return fmt;
    }
    return std::nullopt;
}

}  // anonymous namespace

// ============================================================================
// V4L2 backend
// ============================================================================

#ifdef __linux__

class V4L2CaptureDevice final : public ICaptureDevice {
   public:
    explicit V4L2CaptureDevice(int camera_index);
    ~V4L2CaptureDevice() override;

    V4L2CaptureDevice(const V4L2CaptureDevice&) = delete;
    auto operator=(const V4L2CaptureDevice&) -> V4L2CaptureDevice& = delete;
    V4L2CaptureDevice(V4L2CaptureDevice&&) = delete;
    auto operator=(V4L2CaptureDevice&&) -> V4L2CaptureDevice& = delete;

    [[nodiscard]] auto is_initialized() const -> bool override { return fd_ >= 0; }
    [[nodiscard]] auto capture_frame() -> std::expected<SeetaImageData, CaptureError> override;

   private:
    static constexpr int kDefaultWidth = 640;
    static constexpr int kDefaultHeight = 480;
    static constexpr int kBufferCount = 4;

    int fd_ = -1;
    int camera_index_ = 0;
    int width_ = 0;
    int height_ = 0;
    uint32_t negotiated_fourcc_ = 0;

    struct MappedBuffer {
        void* start = nullptr;
        size_t length = 0;
    };
    std::vector<MappedBuffer> buffers_;
    std::vector<struct v4l2_buffer> buffer_info_;

    auto open_device() -> bool;
    auto negotiate_format() -> bool;
    auto allocate_buffers() -> bool;
    [[nodiscard]] auto start_streaming() const -> bool;
    void stop_streaming() const;

    static void log_format(const char* tag, uint32_t fourcc, int w, int h);
};

V4L2CaptureDevice::V4L2CaptureDevice(int camera_index) : camera_index_(camera_index) {
    if (!open_device()) { return; }
    if (!negotiate_format()) {
        ::close(fd_);
        fd_ = -1;
        return;
    }
    if (!allocate_buffers()) {
        ::close(fd_);
        fd_ = -1;
        return;
    }
    // Queue all buffers before starting
    for (const auto& binfo : buffer_info_) {
        struct v4l2_buffer buf = binfo;
        if (::ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            std::cerr << "V4L2: VIDIOC_QBUF (init) failed: " << std::strerror(errno) << '\n';
            ::close(fd_);
            fd_ = -1;
            return;
        }
    }
    if (!start_streaming()) {
        std::cerr << "V4L2: VIDIOC_STREAMON failed: " << std::strerror(errno) << '\n';
        ::close(fd_);
        fd_ = -1;
    }
}

V4L2CaptureDevice::~V4L2CaptureDevice() {
    if (fd_ < 0) { return; }
    stop_streaming();
    for (auto& mapped_buf : buffers_) {
        if (mapped_buf.start != nullptr && mapped_buf.start != MAP_FAILED) {
            ::munmap(mapped_buf.start, mapped_buf.length);
        }
    }
    buffers_.clear();
    buffer_info_.clear();
    ::close(fd_);
}

auto V4L2CaptureDevice::open_device() -> bool {
    const std::string path = "/dev/video" + std::to_string(camera_index_);
    fd_ = ::open(path.c_str(), O_RDWR);
    if (fd_ < 0) {
        std::cerr << "V4L2: cannot open " << path << ": " << std::strerror(errno) << '\n';
        return false;
    }

    struct v4l2_capability cap = {};
    if (::ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        std::cerr << "V4L2: " << path << " VIDIOC_QUERYCAP failed: " << std::strerror(errno) << '\n';
        return false;
    }

    if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0U) {
        std::cerr << "V4L2: " << path << " does not support VIDEO_CAPTURE\n";
        return false;
    }

    if ((cap.capabilities & V4L2_CAP_STREAMING) == 0U) {
        std::cerr << "V4L2: " << path << " does not support streaming I/O\n";
        return false;
    }

    return true;
}

auto V4L2CaptureDevice::negotiate_format() -> bool {
    static constexpr std::array kPreferredFormats = {V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_NV12};

    // Try each preferred format at the default resolution.
    const auto negotiate = [this](uint32_t pix_fmt) -> bool {
        if (!supports_format(fd_, pix_fmt)) { return false; }
        auto fmt = try_format(fd_, kDefaultWidth, kDefaultHeight, pix_fmt);
        if (!fmt.has_value()) { return false; }
        width_ = static_cast<int>(fmt->fmt.pix.width);
        height_ = static_cast<int>(fmt->fmt.pix.height);
        negotiated_fourcc_ = fmt->fmt.pix.pixelformat;
        return true;
    };

    for (const auto pix_fmt : kPreferredFormats) {
        if (negotiate(pix_fmt)) {
            log_format("V4L2: negotiated", negotiated_fourcc_, width_, height_);
            return true;
        }
    }

    // Fallback: let the driver choose any format.
    if (auto fmt = try_format(fd_, kDefaultWidth, kDefaultHeight, 0)) {
        width_ = static_cast<int>(fmt->fmt.pix.width);
        height_ = static_cast<int>(fmt->fmt.pix.height);
        negotiated_fourcc_ = fmt->fmt.pix.pixelformat;
        log_format("V4L2: driver-chosen", negotiated_fourcc_, width_, height_);
        return true;
    }

    std::cerr << "V4L2: could not negotiate any format\n";
    return false;
}

auto V4L2CaptureDevice::allocate_buffers() -> bool {
    struct v4l2_requestbuffers req = {};
    req.count = kBufferCount;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (::ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "V4L2: VIDIOC_REQBUFS failed: " << std::strerror(errno) << '\n';
        return false;
    }

    if (req.count < 2) {
        std::cerr << "V4L2: insufficient buffers (" << req.count << ")\n";
        return false;
    }

    buffers_.resize(req.count);
    buffer_info_.resize(req.count);

    for (uint32_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (::ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            std::cerr << "V4L2: VIDIOC_QUERYBUF[" << i << "] failed: " << std::strerror(errno) << '\n';
            return false;
        }

        buffers_[i].length = buf.length;
        buffers_[i].start = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);

        if (buffers_[i].start == MAP_FAILED) {
            std::cerr << "V4L2: mmap[" << i << "] failed: " << std::strerror(errno) << '\n';
            return false;
        }

        buffer_info_[i] = buf;
    }

    return true;
}

auto V4L2CaptureDevice::start_streaming() const -> bool {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return ::ioctl(fd_, VIDIOC_STREAMON, &type) >= 0;
}

void V4L2CaptureDevice::stop_streaming() const {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ::ioctl(fd_, VIDIOC_STREAMOFF, &type);
}

void V4L2CaptureDevice::log_format(const char* tag, uint32_t fourcc, int w, int h) {
    std::cerr << tag << ' ' << w << 'x' << h << " fourcc=0x" << std::hex << fourcc << std::dec << '\n';
}

auto V4L2CaptureDevice::capture_frame() -> std::expected<SeetaImageData, CaptureError> {
    if (fd_ < 0) {
        return std::unexpected(CaptureError::kDeviceUnavailable);
    }

    // Wait for frame with poll()
    struct pollfd pfd = {.fd = fd_, .events = static_cast<int16_t>(POLLIN | POLLERR), .revents = 0};
    static constexpr int kPollTimeoutMs = 5000;
    int poll_result = ::poll(&pfd, 1, kPollTimeoutMs);
    if (poll_result < 0) {
        return std::unexpected(errno == EINTR ? CaptureError::kTimeout : CaptureError::kReadFailed);
    }
    if (poll_result == 0) {
        return std::unexpected(CaptureError::kTimeout);
    }

    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (::ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        return std::unexpected(CaptureError::kReadFailed);
    }

    // Convert frame data to RGB24
    const size_t buf_idx = buf.index;
    auto image = build_frame(buffers_[buf_idx].start, width_, negotiated_fourcc_, height_);

    // Re-queue the buffer immediately regardless of conversion result
    ::ioctl(fd_, VIDIOC_QBUF, &buf);

    if (!image.has_value()) {
        return std::unexpected(CaptureError::kReadFailed);
    }

    return *image;
}

// ============================================================================
// PipeWire backend
// ============================================================================

class PipeWireCaptureDevice final : public ICaptureDevice {
   public:
    explicit PipeWireCaptureDevice(int camera_index);
    ~PipeWireCaptureDevice() override;

    PipeWireCaptureDevice(const PipeWireCaptureDevice&) = delete;
    auto operator=(const PipeWireCaptureDevice&) -> PipeWireCaptureDevice& = delete;
    PipeWireCaptureDevice(PipeWireCaptureDevice&&) = delete;
    auto operator=(PipeWireCaptureDevice&&) -> PipeWireCaptureDevice& = delete;

    [[nodiscard]] auto is_initialized() const -> bool override { return initialized_; }
    [[nodiscard]] auto capture_frame() -> std::expected<SeetaImageData, CaptureError> override;

   private:
    static constexpr int kDefaultWidth = 640;
    static constexpr int kDefaultHeight = 480;
    static constexpr std::size_t kParamsBufferSize = 1024;
    static constexpr int kConnectTimeoutSec = 2;
    static constexpr int kCaptureTimeoutSec = 5;

    int camera_index_ = 0;
    bool initialized_ = false;
    bool frame_ready_ = false;
    bool stream_error_ = false;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    uint32_t stream_fourcc_ = 0;

    struct pw_stream_events stream_events_ = {};
    struct spa_hook stream_listener_ = {};

    std::vector<uint8_t> latest_frame_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;

    static void on_process_cb(void* user_data);
    static void on_stream_state_cb(void* user_data, pw_stream_state old_state, pw_stream_state new_state,
                                   const char* error);
    static void on_param_changed_cb(void* user_data, uint32_t param_id, const struct spa_pod* param);

    auto try_init() -> bool;
    auto try_connect() -> bool;
    void cleanup();
};

PipeWireCaptureDevice::PipeWireCaptureDevice(int camera_index) : camera_index_(camera_index) {
    (void)camera_index_;
    if (!try_init()) { return; }
    if (!try_connect()) {
        cleanup();
        return;
    }
    initialized_ = true;
}

PipeWireCaptureDevice::~PipeWireCaptureDevice() {
    cleanup();
}

auto PipeWireCaptureDevice::try_init() -> bool {
    [[maybe_unused]] static const bool s_pw_initialized = []() {
        pw_init(nullptr, nullptr);
        return true;
    }();

    loop_ = pw_thread_loop_new("camera-pw-loop", nullptr);
    if (loop_ == nullptr) { return false; }

    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (context_ == nullptr) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
        return false;
    }

    core_ = pw_context_connect(context_, nullptr, 0);
    if (core_ == nullptr) {
        pw_context_destroy(context_);
        context_ = nullptr;
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
        return false;
    }

    return true;
}

void PipeWireCaptureDevice::cleanup() {
    if (context_ != nullptr) {
        pw_context_destroy(context_);
        context_ = nullptr;
        core_ = nullptr;
        stream_ = nullptr;
    }
    if (loop_ != nullptr) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
}

auto PipeWireCaptureDevice::try_connect() -> bool {
    stream_ = pw_stream_new(core_, "camera-capture", nullptr);
    if (stream_ == nullptr) { return false; }

    stream_events_.version = PW_VERSION_STREAM_EVENTS;
    stream_events_.process = on_process_cb;
    stream_events_.state_changed = on_stream_state_cb;
    stream_events_.param_changed = on_param_changed_cb;

    pw_stream_add_listener(stream_, &stream_listener_, &stream_events_, this);

    // Build format params: request YUY2 at 640×480
    auto params_buffer = std::array<std::uint8_t, kParamsBufferSize>{};
    struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(params_buffer.data(), params_buffer.size());

    struct spa_rectangle rect_size = SPA_RECTANGLE(kDefaultWidth, kDefaultHeight);
    struct spa_pod_frame pod_frame = {};
    spa_pod_builder_push_object(&pod_builder, &pod_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&pod_builder,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_YUY2),
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&rect_size),
        0);
    spa_pod_builder_pop(&pod_builder, &pod_frame);

    const auto* pod = spa_pod_builder_deref(&pod_builder, pod_frame.offset);
    // NOLINTNEXTLINE(modernize-avoid-c-arrays): PipeWire C API requires C array
    const struct spa_pod* params[1] = {pod};

    auto flags = static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
    int res = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, pod != nullptr ? 1 : 0);

    if (res < 0) {
        std::cerr << "PipeWire: pw_stream_connect failed: " << spa_strerror(res) << '\n';
        return false;
    }

    pw_thread_loop_start(loop_);

    pw_thread_loop_lock(loop_);
    while (!stream_error_ && pw_stream_get_state(stream_, nullptr) != PW_STREAM_STATE_STREAMING) {
        if (pw_thread_loop_timed_wait(loop_, kConnectTimeoutSec) != 0) { break; }
    }
    pw_thread_loop_unlock(loop_);

    if (stream_error_ || pw_stream_get_state(stream_, nullptr) != PW_STREAM_STATE_STREAMING) {
        std::cerr << "PipeWire: stream did not reach streaming state\n";
        return false;
    }

    if (stream_fourcc_ == 0) {
        std::cerr << "PipeWire: stream negotiated with unknown format\n";
        return false;
    }

    return true;
}

void PipeWireCaptureDevice::on_param_changed_cb(void* user_data, uint32_t param_id, const struct spa_pod* param) {
    auto* self = static_cast<PipeWireCaptureDevice*>(user_data);
    if (param == nullptr || param_id != SPA_PARAM_Format) { return; }

    struct spa_video_info_raw info = {};
    if (spa_format_video_raw_parse(param, &info) == 0) {
        self->width_ = static_cast<int>(info.size.width);
        self->height_ = static_cast<int>(info.size.height);

        switch (info.format) {
            case SPA_VIDEO_FORMAT_YUY2:
                self->stream_fourcc_ = V4L2_PIX_FMT_YUYV;
                break;
            case SPA_VIDEO_FORMAT_NV12:
                self->stream_fourcc_ = V4L2_PIX_FMT_NV12;
                break;
            default:
                self->stream_fourcc_ = 0;
                break;
        }
    }
}

void PipeWireCaptureDevice::on_stream_state_cb(void* user_data, pw_stream_state /*old_state*/,
                                                pw_stream_state new_state, const char* error) {
    auto* self = static_cast<PipeWireCaptureDevice*>(user_data);
    if (new_state == PW_STREAM_STATE_ERROR) {
        self->stream_error_ = true;
        if (error != nullptr) {
            std::cerr << "PipeWire: stream error: " << error << '\n';
        }
        pw_thread_loop_signal(self->loop_, false);
    }
    if (new_state == PW_STREAM_STATE_STREAMING) {
        pw_thread_loop_signal(self->loop_, false);
    }
}

void PipeWireCaptureDevice::on_process_cb(void* user_data) {
    auto* self = static_cast<PipeWireCaptureDevice*>(user_data);

    auto* buf = pw_stream_dequeue_buffer(self->stream_);
    if (buf == nullptr) { return; }

    const auto& buffer_data = buf->buffer->datas[0];
    if (buffer_data.data == nullptr || buffer_data.chunk->size == 0) {
        pw_stream_queue_buffer(self->stream_, buf);
        return;
    }

    {
        std::scoped_lock lock(self->frame_mutex_);
        const auto* raw = static_cast<const uint8_t*>(buffer_data.data);
        self->latest_frame_.assign(raw, raw + buffer_data.chunk->size);
        self->frame_ready_ = true;
    }

    pw_stream_queue_buffer(self->stream_, buf);
    self->frame_cv_.notify_one();
}

auto PipeWireCaptureDevice::capture_frame() -> std::expected<SeetaImageData, CaptureError> {
    if (!initialized_) {
        return std::unexpected(CaptureError::kDeviceUnavailable);
    }

    std::unique_lock lock(frame_mutex_);
    frame_ready_ = false;

    if (!frame_cv_.wait_for(lock, std::chrono::seconds(kCaptureTimeoutSec),
                            [this] { return frame_ready_ || stream_error_; }))
    {
        return std::unexpected(CaptureError::kTimeout);
    }

    if (stream_error_ || latest_frame_.empty()) {
        return std::unexpected(CaptureError::kReadFailed);
    }

    auto image = build_frame(latest_frame_.data(), width_ > 0 ? width_ : kDefaultWidth,
                             stream_fourcc_, height_ > 0 ? height_ : kDefaultHeight);

    if (!image.has_value()) {
        return std::unexpected(CaptureError::kReadFailed);
    }

    return *image;
}

#endif  // __linux__

// ============================================================================
// Null backend (fallback for unsupported platforms)
// ============================================================================

class NullCaptureDevice final : public ICaptureDevice {
   public:
    [[nodiscard]] auto is_initialized() const -> bool override { return false; }
    [[nodiscard]] auto capture_frame() -> std::expected<SeetaImageData, CaptureError> override {
        return std::unexpected(CaptureError::kDeviceUnavailable);
    }
};

}  // namespace su::facerecognizer

// ============================================================================
// Exported interface
// ============================================================================

export namespace su::facerecognizer {

auto create_capture_device(int camera_index) -> std::unique_ptr<ICaptureDevice> {
#ifdef __linux__
    {
        auto dev = std::make_unique<PipeWireCaptureDevice>(camera_index);
        if (dev->is_initialized()) {
            std::cerr << "Camera: using PipeWire backend (camera " << camera_index << ")\n";
            return dev;
        }
    }
    {
        auto dev = std::make_unique<V4L2CaptureDevice>(camera_index);
        if (dev->is_initialized()) {
            std::cerr << "Camera: using V4L2 backend (camera " << camera_index << ")\n";
            return dev;
        }
    }
    std::cerr << "Camera: no backend available for camera " << camera_index << '\n';
#else
    std::cerr << "Camera: no Linux backend (unsupported platform)\n";
#endif
    return std::make_unique<NullCaptureDevice>();
}

auto enumerate_cameras() -> std::vector<std::string> {
    std::vector<std::string> result;
#ifdef __linux__
    for (int dev = 0; dev < 64; ++dev) {
        const std::string path = "/dev/video" + std::to_string(dev);
        int dev_fd = ::open(path.c_str(), O_RDWR);
        if (dev_fd < 0) { continue; }

        struct v4l2_capability cap = {};
        if (::ioctl(dev_fd, VIDIOC_QUERYCAP, &cap) == 0 && (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0U) {
            result.emplace_back(path);
        }
        ::close(dev_fd);
    }
#endif
    if (result.empty()) {
        result.emplace_back("camera#0");
    }
    return result;
}

}  // namespace su::facerecognizer
