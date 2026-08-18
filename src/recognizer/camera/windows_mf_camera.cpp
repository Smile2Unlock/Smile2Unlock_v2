// Windows Media Foundation camera backend (plain TU; see windows_mf_camera.h).
//
// Design:
// - All Media Foundation calls (CoInitializeEx, MFStartup, device
//   enumeration, source reader) run on a private MTA worker thread. The
//   source reader is apartment-affine; routing every call through one worker
//   keeps it on a stable MTA regardless of the caller's apartment, and the
//   synchronous request/response protocol makes open/grab thread-safe.
// - Frames are captured as YUY2 (identical byte packing to V4L2 YUYV) when
//   the device supports it, falling back to the native subtype (MJPEG is
//   passed through as-is; other subtypes are reported as unavailable).
// - The frame buffer is copied into a stream-owned buffer so the returned
//   pointer stays valid until the next grab (mirrors the V4L2 mmap contract).

#include "windows_mf_camera.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace su::recognizer {

namespace {

// V4L2_PIX_FMT values understood by v4l2_frame_to_rgb.
constexpr std::uint32_t kV4l2Yuyv = 0x56595559; // 'YUYV'
constexpr std::uint32_t kV4l2Mjpg = 0x47504A4D; // 'MJPG'

bool is_supported_subtype(const GUID& subtype) {
    return subtype == MFVideoFormat_YUY2 || subtype == MFVideoFormat_MJPG;
}

std::uint32_t v4l2_format_for(const GUID& subtype) {
    if (subtype == MFVideoFormat_YUY2) {
        return kV4l2Yuyv;
    }
    if (subtype == MFVideoFormat_MJPG) {
        return kV4l2Mjpg;
    }
    return 0;
}

std::string wide_to_utf8(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int length = ::WideCharToMultiByte(
        CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length - 1), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, wide, -1, result.data(), length, nullptr, nullptr);
    return result;
}

// One-shot MF platform init for the worker thread. Returns false when COM or
// Media Foundation cannot start (never fatal for the caller; camera is
// reported unavailable).
bool initialize_mf() {
    if (FAILED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        return false;
    }
    if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        ::CoUninitialize();
        return false;
    }
    return true;
}

void shutdown_mf() {
    ::MFShutdown();
    ::CoUninitialize();
}

// MFEnumDeviceSources is missing from the mingw-w64 mfplat import library;
// resolve it at runtime. It is exported by mf.dll (not mfplat.dll) on
// Windows 8+ — loading it from mfplat returns NULL and enumeration silently
// reports no cameras even when a UVC device is present (observed on a KVM VM
// where Device Manager shows "USB2.0 HD UVC WebCam" but MF enumerated none).
using MfEnumDeviceSourcesFn =
    HRESULT(WINAPI*)(IMFAttributes*, IMFActivate***, UINT32*);

MfEnumDeviceSourcesFn load_mf_enum_device_sources() {
    static const MfEnumDeviceSourcesFn fn = [] {
        HMODULE mf = ::LoadLibraryW(L"mf.dll");
        if (mf == nullptr) {
            return static_cast<MfEnumDeviceSourcesFn>(nullptr);
        }
        return reinterpret_cast<MfEnumDeviceSourcesFn>(
            ::GetProcAddress(mf, "MFEnumDeviceSources"));
    }();
    return fn;
}

bool enumerate_devices(std::vector<ComPtr<IMFActivate>>& out) {
    const auto mf_enum_device_sources = load_mf_enum_device_sources();
    if (mf_enum_device_sources == nullptr) {
        return false;
    }
    ComPtr<IMFAttributes> attributes;
    if (FAILED(::MFCreateAttributes(&attributes, 1))) {
        return false;
    }
    if (FAILED(attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
        return false;
    }
    IMFActivate** raw_devices = nullptr;
    UINT32 count = 0;
    if (FAILED(mf_enum_device_sources(
            attributes.Get(), &raw_devices, &count))) {
        return false;
    }
    out.clear();
    out.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        out.emplace_back(raw_devices[i]);
    }
    ::CoTaskMemFree(raw_devices);
    return true;
}

} // namespace

std::vector<MfCameraDeviceInfo> mf_enumerate_cameras() {
    // Enumeration is stateless and cheap; run it on a scratch worker thread
    // so it never depends on the caller's apartment.
    std::vector<MfCameraDeviceInfo> result;
    std::thread worker([&result] {
        if (!initialize_mf()) {
            return;
        }
        std::vector<ComPtr<IMFActivate>> devices;
        if (enumerate_devices(devices)) {
            for (std::size_t i = 0; i < devices.size(); ++i) {
                MfCameraDeviceInfo info;
                info.index = static_cast<int>(i);
                PWSTR name = nullptr;
                UINT32 name_length = 0;
                if (SUCCEEDED(devices[i]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                        &name,
                        &name_length))) {
                    info.name = wide_to_utf8(name);
                    ::CoTaskMemFree(name);
                }
                PWSTR link = nullptr;
                UINT32 link_length = 0;
                if (SUCCEEDED(devices[i]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                        &link,
                        &link_length))) {
                    info.symbolic_link = wide_to_utf8(link);
                    ::CoTaskMemFree(link);
                }
                result.push_back(std::move(info));
            }
        }
        shutdown_mf();
    });
    worker.join();
    return result;
}

class MfCameraStream::Impl {
public:
    Impl() = default;
    ~Impl() { close(); }

    bool open(int index) {
        std::unique_lock lock(mutex_);
        if (!ensure_worker(lock)) {
            return false;
        }
        op_ = Op::Open;
        open_index_ = index;
        done_ = false;
        cv_.notify_all();
        cv_.wait(lock, [this] { return done_; });
        return open_result_;
    }

    bool grab(MfCameraFrame& out) {
        std::unique_lock lock(mutex_);
        if (!worker_alive_ || !open_) {
            return false;
        }
        op_ = Op::Grab;
        done_ = false;
        cv_.notify_all();
        cv_.wait(lock, [this] { return done_; });
        if (!grab_result_) {
            return false;
        }
        out = frame_;
        return true;
    }

    void close() {
        std::unique_lock lock(mutex_);
        if (!worker_alive_) {
            return;
        }
        op_ = Op::Quit;
        done_ = false;
        cv_.notify_all();
        cv_.wait(lock, [this] { return done_; });
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool is_open() const {
        std::lock_guard lock(mutex_);
        return worker_alive_ && open_;
    }

private:
    enum class Op { None, Open, Grab, Quit };

    bool ensure_worker(std::unique_lock<std::mutex>& lock) {
        if (worker_alive_) {
            return true;
        }
        worker_ = std::thread([this] { worker_main(); });
        // Wait for the worker to finish platform init.
        cv_.wait(lock, [this] { return worker_ready_ || worker_failed_; });
        return worker_ready_;
    }

    void worker_main() {
        const bool mf_ok = initialize_mf();
        {
            std::lock_guard lock(mutex_);
            worker_alive_ = true;
            worker_ready_ = mf_ok;
            worker_failed_ = !mf_ok;
            cv_.notify_all();
        }
        for (;;) {
            Op op;
            int index = -1;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return op_ != Op::None; });
                op = op_;
                index = open_index_;
            }
            if (op == Op::None) {
                continue;
            }
            if (op == Op::Quit) {
                close_worker();
                {
                    std::lock_guard lock(mutex_);
                    op_ = Op::None;
                    done_ = true;
                    cv_.notify_all();
                }
                if (mf_ok) {
                    shutdown_mf();
                }
                return;
            }
            if (!mf_ok) {
                if (op == Op::Open) {
                    finish_open(false);
                } else if (op == Op::Grab) {
                    finish_grab(false);
                }
                continue;
            }
            switch (op) {
            case Op::Open:
                open_worker(index);
                break;
            case Op::Grab:
                grab_worker();
                break;
            case Op::None:
            case Op::Quit:
                break;
            }
        }
    }

    void open_worker(int index) {
        close_worker();
        open_ = false;
        std::vector<ComPtr<IMFActivate>> devices;
        if (!enumerate_devices(devices)
            || index < 0 || static_cast<std::size_t>(index) >= devices.size()) {
            finish_open(false);
            return;
        }
        ComPtr<IMFMediaSource> source;
        if (FAILED(devices[static_cast<std::size_t>(index)]->ActivateObject(
                IID_PPV_ARGS(&source)))) {
            finish_open(false);
            return;
        }
        ComPtr<IMFSourceReader> reader;
        if (FAILED(::MFCreateSourceReaderFromMediaSource(
                source.Get(), nullptr, &reader))) {
            finish_open(false);
            return;
        }

        // Prefer YUY2 (same packing as V4L2 YUYV). Some devices — notably
        // virtual cameras behind a KVM (QEMU/usbredir) — return a zero
        // MF_MT_FRAME_SIZE when the media type only pins the subtype. Set an
        // explicit resolution so negotiation yields a real size; otherwise the
        // open fails even though the device enumerates fine.
        const auto try_media_type = [&](const GUID& subtype, std::uint64_t size) {
            ComPtr<IMFMediaType> requested;
            if (FAILED(::MFCreateMediaType(&requested))) {
                return false;
            }
            if (FAILED(requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
                || FAILED(requested->SetGUID(MF_MT_SUBTYPE, subtype))
                || FAILED(requested->SetUINT64(MF_MT_FRAME_SIZE, size))) {
                return false;
            }
            return SUCCEEDED(reader->SetCurrentMediaType(0, nullptr, requested.Get()));
        };
        const auto packed_size = [](std::uint32_t w, std::uint32_t h) {
            return (static_cast<std::uint64_t>(w) << 32) | static_cast<std::uint64_t>(h);
        };
        static constexpr std::array kSizes = std::array{
            packed_size(1920, 1080), packed_size(1280, 720), packed_size(960, 540),
            packed_size(640, 480), packed_size(320, 240), packed_size(176, 144),
        };
        bool type_set = false;
        for (const auto size : kSizes) {
            if (try_media_type(MFVideoFormat_YUY2, size)) {
                type_set = true;
                break;
            }
        }
        if (!type_set) {
            for (const auto size : kSizes) {
                if (try_media_type(MFVideoFormat_MJPG, size)) {
                    type_set = true;
                    break;
                }
            }
        }
        if (!type_set) {
            // Last resort: let the reader pick any format/ratio.
            ComPtr<IMFMediaType> requested;
            if (SUCCEEDED(::MFCreateMediaType(&requested))
                && SUCCEEDED(requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))) {
                type_set = SUCCEEDED(reader->SetCurrentMediaType(0, nullptr, requested.Get()));
            }
        }
        if (!type_set) {
            finish_open(false);
            return;
        }

        ComPtr<IMFMediaType> negotiated;
        if (FAILED(reader->GetCurrentMediaType(0, &negotiated))) {
            finish_open(false);
            return;
        }
        GUID major{};
        GUID subtype{};
        if (FAILED(negotiated->GetGUID(MF_MT_MAJOR_TYPE, &major))
            || major != MFMediaType_Video
            || FAILED(negotiated->GetGUID(MF_MT_SUBTYPE, &subtype))
            || !is_supported_subtype(subtype)) {
            finish_open(false);
            return;
        }
        // MF_MT_FRAME_SIZE is a UINT64: width occupies the high 32 bits and
        // height the low 32 bits.
        UINT64 frame_size = 0;
        if (FAILED(negotiated->GetUINT64(MF_MT_FRAME_SIZE, &frame_size))
            || frame_size == 0) {
            finish_open(false);
            return;
        }
        const UINT32 width = static_cast<UINT32>(frame_size >> 32);
        const UINT32 height = static_cast<UINT32>(frame_size & 0xFFFF'FFFFULL);
        if (width == 0 || height == 0) {
            finish_open(false);
            return;
        }
        UINT32 raw_stride = 0;
        INT32 stride = 0;
        if (FAILED(negotiated->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) {
            stride = static_cast<INT32>(width * 2); // packed 4:2:2
        } else {
            stride = static_cast<INT32>(raw_stride);
        }
        reader_ = std::move(reader);
        width_ = width;
        height_ = height;
        stride_ = static_cast<std::size_t>(
            stride < 0 ? -static_cast<std::int64_t>(stride) : stride);
        if (stride_ == 0) {
            stride_ = static_cast<std::size_t>(width) * 2;
        }
        v4l2_format_ = v4l2_format_for(subtype);
        bottom_up_ = stride < 0 && v4l2_format_ == kV4l2Yuyv;
        open_ = true;
        finish_open(true);
    }

    void grab_worker() {
        grab_result_ = false;
        if (!open_ || !reader_) {
            finish_grab(false);
            return;
        }
        for (int attempts = 0; attempts < 32; ++attempts) {
            DWORD stream_index = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            const HRESULT hr = reader_->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0,
                &stream_index,
                &flags,
                &timestamp,
                &sample);
            if (FAILED(hr)) {
                finish_grab(false);
                return;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                finish_grab(false);
                return;
            }
            if ((flags & MF_SOURCE_READERF_NEWSTREAM) != 0
                || (flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
                continue;
            }
            if (!sample) {
                continue;
            }
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
                finish_grab(false);
                return;
            }
            BYTE* data = nullptr;
            DWORD current = 0;
            DWORD max = 0;
            if (FAILED(buffer->Lock(&data, &max, &current)) || data == nullptr) {
                finish_grab(false);
                return;
            }
            const auto image_bytes = stride_ * static_cast<std::size_t>(height_);
            if (bottom_up_ && stride_ != 0 && current >= image_bytes) {
                grab_buffer_.resize(current);
                for (std::size_t row = 0; row < height_; ++row) {
                    std::memcpy(
                        grab_buffer_.data() + row * stride_,
                        data + (static_cast<std::size_t>(height_) - 1 - row) * stride_,
                        stride_);
                }
                if (current > image_bytes) {
                    std::memcpy(
                        grab_buffer_.data() + image_bytes,
                        data + image_bytes,
                        current - image_bytes);
                }
            } else {
                grab_buffer_.assign(data, data + current);
            }
            buffer->Unlock();
            frame_.width = static_cast<int>(width_);
            frame_.height = static_cast<int>(height_);
            frame_.stride = stride_;
            frame_.v4l2_format = v4l2_format_;
            frame_.bytes = grab_buffer_.data();
            frame_.byte_count = grab_buffer_.size();
            grab_result_ = true;
            break;
        }
        finish_grab(grab_result_);
    }

    void close_worker() {
        reader_.Reset();
        open_ = false;
    }

    void finish_open(bool ok) {
        std::lock_guard lock(mutex_);
        open_result_ = ok;
        op_ = Op::None;
        done_ = true;
        cv_.notify_all();
    }

    void finish_grab(bool ok) {
        std::lock_guard lock(mutex_);
        grab_result_ = ok;
        op_ = Op::None;
        done_ = true;
        cv_.notify_all();
    }

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool worker_ready_ = false;
    bool worker_failed_ = false;
    bool worker_alive_ = false;
    bool open_ = false;
    Op op_ = Op::None;
    int open_index_ = -1;
    bool open_result_ = false;
    bool grab_result_ = false;
    bool done_ = false;
    MfCameraFrame frame_;
    std::vector<std::uint8_t> grab_buffer_;

    // Worker-thread-owned MF state.
    ComPtr<IMFSourceReader> reader_;
    UINT32 width_ = 0;
    UINT32 height_ = 0;
    std::size_t stride_ = 0;
    bool bottom_up_ = false;
    std::uint32_t v4l2_format_ = 0;
};

MfCameraStream::MfCameraStream() : impl_(std::make_unique<Impl>()) {}
MfCameraStream::~MfCameraStream() = default;

bool MfCameraStream::open(int index) {
    return impl_->open(index);
}

bool MfCameraStream::grab(MfCameraFrame& out) {
    return impl_->grab(out);
}

void MfCameraStream::close() {
    if (impl_) {
        impl_->close();
    }
}

bool MfCameraStream::is_open() const {
    return impl_ && impl_->is_open();
}

} // namespace su::recognizer
