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

#include <atomic>
#include <chrono>
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

// Grab-completion sink implemented by MfCameraStream::Impl. A plain
// namespace-scope interface keeps the reader callback independent of the
// private nested Impl name.
class MfGrabSink {
public:
    virtual ~MfGrabSink() = default;
    virtual void on_read_sample(HRESULT status, DWORD flags, IMFSample* sample) = 0;
};

// Async IMFSourceReader callback. The reader is created in async mode so a
// grab is a request + bounded wait instead of a synchronous ReadSample that
// can block forever when a (virtual) camera stops delivering frames — the
// old sync path left the worker stuck mid-call, so a pending Quit could
// never be processed and stopping the camera hung the UI thread.
// MF invokes OnReadSample on one of its work-queue threads; it only touches
// the per-request completion state below, guarded by the grab mutex, and
// never the op-dispatch mutex.
class StreamCallback final : public IMFSourceReaderCallback {
public:
    explicit StreamCallback(MfGrabSink* sink) : sink_(sink) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        // __uuidof rather than the IID_ constants: mingw's import libraries
        // do not carry every IID symbol.
        if (riid == __uuidof(IUnknown)
            || riid == __uuidof(IMFSourceReaderCallback)) {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return references_.fetch_add(1) + 1;
    }
    STDMETHODIMP_(ULONG) Release() override {
        const auto remaining = references_.fetch_sub(1) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    // IMFSourceReaderCallback
    STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG,
                              IMFSample* sample) override;
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    MfGrabSink* sink_;
    std::atomic<ULONG> references_{1};
};

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

class MfCameraStream::Impl final : public MfGrabSink {
public:
    Impl() = default;
    ~Impl() { close(); }

    // MfGrabSink: called by StreamCallback on an MF work-queue thread.
    void on_read_sample(HRESULT status, DWORD flags, IMFSample* sample) override {
        std::lock_guard lock(grab_mutex_);
        if (!grab_pending_) {
            return; // request already timed out and was cancelled
        }
        grab_done_ = true;
        grab_pending_ = false;
        sample_status_ = status;
        sample_flags_ = flags;
        // ComPtr assignment from a raw pointer attaches without AddRef, but
        // MF drops its reference as soon as OnReadSample returns — take our
        // own reference so the sample survives until the worker consumes it.
        if (sample != nullptr) {
            sample->AddRef();
        }
        sample_ = sample;
        grab_cv_.notify_all();
    }

    bool open(int index) {
        std::unique_lock lock(mutex_);
        if (quit_pending_ || !ensure_worker(lock)) {
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
        if (!worker_alive_ || !open_ || quit_pending_) {
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
        // quit_pending_ stops the grab loop from re-submitting Op::Grab after
        // the worker finished the in-flight one: without it the capture thread
        // can overwrite a pending Quit (or keep the worker busy forever) and
        // close()'s join() never returns — the camera-stop UI freeze.
        quit_pending_ = true;
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

    // Per-read cap: bounds how long the worker can be inside one grab when a
    // device stalls, and therefore how long a Quit (camera stop) can be
    // delayed before the UI sees the worker exit. The drain window after
    // Flush() is shorter: MF completes a flushed request promptly.
    static constexpr auto kReadDeadline = std::chrono::milliseconds(500);
    static constexpr auto kCancelDrain = std::chrono::milliseconds(200);

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
                    // Publish the shutdown atomically so a grab() that lost
                    // the race sees a dead worker instead of resubmitting,
                    // and a subsequent open() may start a fresh worker.
                    quit_pending_ = false;
                    worker_alive_ = false;
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
        // Async reader: hand MF our callback so reads are cancellable waits
        // instead of unbounded synchronous calls.
        callback_ = new StreamCallback(static_cast<MfGrabSink*>(this));
        ComPtr<IMFAttributes> attributes;
        ComPtr<IMFSourceReader> reader;
        if (FAILED(::MFCreateAttributes(&attributes, 1))
            || FAILED(attributes->SetUnknown(
                   MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get()))
            || FAILED(::MFCreateSourceReaderFromMediaSource(
                   source.Get(), attributes.Get(), &reader))) {
            callback_.Reset();
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
            {
                std::lock_guard lock(grab_mutex_);
                grab_done_ = false;
                grab_pending_ = true;
                sample_ = nullptr;
                sample_status_ = E_FAIL;
                sample_flags_ = 0;
            }
            // Async request: all out-params must be null in callback mode.
            // The reader allows one outstanding request per stream, and we
            // never issue the next one before this one completes (or is
            // cancelled below), so requests can never overlap.
            const HRESULT requested = reader_->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr,
                nullptr, nullptr);
            if (FAILED(requested)) {
                std::lock_guard lock(grab_mutex_);
                grab_pending_ = false;
                finish_grab(false);
                return;
            }
            ComPtr<IMFSample> held;
            IMFSample* sample = nullptr;
            DWORD flags = 0;
            {
                std::unique_lock lock(grab_mutex_);
                if (!grab_cv_.wait_for(lock, kReadDeadline, [this] {
                        return grab_done_;
                    })) {
                    // Device stalled (virtual camera stopped feeding, driver
                    // wedged). Cancel the outstanding request and fail the
                    // grab so the op loop can process a pending Quit.
                    lock.unlock();
                    (void)reader_->Flush(MF_SOURCE_READER_ALL_STREAMS);
                    lock.lock();
                    (void)grab_cv_.wait_for(lock, kCancelDrain, [this] {
                        return grab_done_;
                    });
                    grab_pending_ = false;
                    finish_grab(false);
                    return;
                }
                grab_pending_ = false;
                if (FAILED(sample_status_)) {
                    finish_grab(false);
                    return;
                }
                flags = sample_flags_;
                // ComPtr-to-ComPtr copy AddRefs, so the reference the
                // callback took stays balanced when we clear sample_ here and
                // again when `held` below goes out of scope.
                held = sample_;
                sample_ = nullptr;
                sample = held.Get();
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                finish_grab(false);
                return;
            }
            if ((flags & MF_SOURCE_READERF_NEWSTREAM) != 0
                || (flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
                continue;
            }
            if (sample == nullptr) {
                continue;
            }
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(held->ConvertToContiguousBuffer(&buffer))) {
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
        // Flush first so a request still outstanding on a stalled device is
        // completed/cancelled by MF; releasing a reader with a pending request
        // can otherwise block inside the driver.
        if (reader_) {
            (void)reader_->Flush(MF_SOURCE_READER_ALL_STREAMS);
        }
        reader_.Reset();
        callback_.Reset();
        open_ = false;
    }

    void finish_open(bool ok) {
        std::lock_guard lock(mutex_);
        open_result_ = ok;
        // Clear only our own op: a Quit (or a newer op) posted by the caller
        // while the worker was executing must survive, or the worker parks in
        // its dispatch wait forever and close()'s join() never returns — the
        // camera-stop UI freeze.
        if (op_ == Op::Open) {
            op_ = Op::None;
        }
        done_ = true;
        cv_.notify_all();
    }

    void finish_grab(bool ok) {
        std::lock_guard lock(mutex_);
        grab_result_ = ok;
        // Same guard as finish_open: a concurrent close() sets Op::Quit while
        // this grab is in flight; clobbering it loses the stop request.
        if (op_ == Op::Grab) {
            op_ = Op::None;
        }
        done_ = true;
        cv_.notify_all();
    }

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool worker_ready_ = false;
    bool worker_failed_ = false;
    bool worker_alive_ = false;
    // Set by close() until the worker has fully exited; grab()/open() refuse
    // to submit while it is set so a pending Quit can never be overwritten.
    bool quit_pending_ = false;
    bool open_ = false;
    Op op_ = Op::None;
    int open_index_ = -1;
    bool open_result_ = false;
    bool grab_result_ = false;
    bool done_ = false;
    MfCameraFrame frame_;
    std::vector<std::uint8_t> grab_buffer_;

    // Per-request completion state, signalled by StreamCallback on an MF
    // work-queue thread. grab_pending_ also fences late deliveries: a
    // callback firing after the request was cancelled finds it false and
    // drops the sample instead of corrupting the next grab.
    std::mutex grab_mutex_;
    std::condition_variable grab_cv_;
    bool grab_done_ = false;
    bool grab_pending_ = false;
    HRESULT sample_status_ = E_FAIL;
    DWORD sample_flags_ = 0;
    ComPtr<IMFSample> sample_;

    // Worker-thread-owned MF state.
    ComPtr<IMFSourceReader> reader_;
    ComPtr<StreamCallback> callback_;
    UINT32 width_ = 0;
    UINT32 height_ = 0;
    std::size_t stride_ = 0;
    bool bottom_up_ = false;
    std::uint32_t v4l2_format_ = 0;
};

STDMETHODIMP StreamCallback::OnReadSample(
    HRESULT status, DWORD stream_index, DWORD flags, LONGLONG timestamp,
    IMFSample* sample) {
    (void)stream_index;
    (void)timestamp;
    sink_->on_read_sample(status, flags, sample);
    return S_OK;
}

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
