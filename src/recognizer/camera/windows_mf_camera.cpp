#include "common/log/logger.h"
// Windows Media Foundation camera backend (plain TU; see windows_mf_camera.h).
//
// Design:
// - All Media Foundation calls (CoInitializeEx, MFStartup, device
//   enumeration, source reader) run on a private MTA worker thread. The
//   source reader is apartment-affine; routing every call through one worker
//   keeps it on a stable MTA regardless of the caller's apartment, and the
//   synchronous request/response protocol makes open/grab thread-safe.
// - Native MJPEG/YUY2/NV12 modes are ranked by cadence and recognition-sized
//   resolution. MJPEG avoids saturating USB with uncompressed HD video.
// - The frame buffer is copied into a stream-owned buffer so the returned
//   pointer stays valid until the next grab (mirrors the V4L2 mmap contract).

#include "windows_mf_camera.h"
#include "capture_mode.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace su::recognizer {

namespace {

// V4L2_PIX_FMT values understood by v4l2_frame_to_rgb.
constexpr std::uint32_t kV4l2Yuyv = 0x56595559; // 'YUYV'
constexpr std::uint32_t kV4l2Mjpg = 0x47504A4D; // 'MJPG'
constexpr std::uint32_t kV4l2Nv12 = 0x3231564E; // 'NV12'

bool is_supported_subtype(const GUID& subtype) {
    return subtype == MFVideoFormat_YUY2 || subtype == MFVideoFormat_MJPG
        || subtype == MFVideoFormat_NV12;
}

std::uint32_t v4l2_format_for(const GUID& subtype) {
    if (subtype == MFVideoFormat_YUY2) {
        return kV4l2Yuyv;
    }
    if (subtype == MFVideoFormat_MJPG) {
        return kV4l2Mjpg;
    }
    if (subtype == MFVideoFormat_NV12) {
        return kV4l2Nv12;
    }
    return 0;
}

bool permission_or_busy(HRESULT hr) {
    return hr == E_ACCESSDENIED
        || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)
        || hr == HRESULT_FROM_WIN32(ERROR_BUSY)
        || hr == MF_E_VIDEO_DEVICE_LOCKED
        || hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED
#ifdef MF_E_VIDEO_RECORDING_DEVICE_LOCKED
        || hr == MF_E_VIDEO_RECORDING_DEVICE_LOCKED
#endif
        ;
}

void log_open_failure(std::string_view step, HRESULT hr) {
    char hex[11]{};
    std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(hr));
    su::log::process_logger().warn("camera.open_failed", {
        su::log::field("step", step), su::log::field("hresult", hex),
        su::log::field("permission_or_busy", permission_or_busy(hr) ? 1 : 0)});
}

// Log only the media subtype, never device names or symbolic links. Some
// subtypes are GUIDs rather than fourccs; retain their numeric Data1 as well.
std::string subtype_fourcc(const GUID& subtype) {
    std::string result(4, '.');
    for (unsigned i = 0; i < 4; ++i) {
        const auto ch = (subtype.Data1 >> (i * 8)) & 0xff;
        if (ch >= 32 && ch <= 126) {
            result[i] = static_cast<char>(ch);
        }
    }
    return result;
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
    const auto com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) {
        log_open_failure("co_initialize", com);
        return false;
    }
    const auto startup = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(startup)) {
        log_open_failure("mf_startup", startup);
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

HRESULT enumerate_devices(std::vector<ComPtr<IMFActivate>>& out) {
    const auto mf_enum_device_sources = load_mf_enum_device_sources();
    if (mf_enum_device_sources == nullptr) {
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    ComPtr<IMFAttributes> attributes;
    auto hr = ::MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) {
        return hr;
    }
    hr = attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
        return hr;
    }
    IMFActivate** raw_devices = nullptr;
    UINT32 count = 0;
    hr = mf_enum_device_sources(attributes.Get(), &raw_devices, &count);
    if (FAILED(hr)) {
        return hr;
    }
    out.clear();
    out.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IMFActivate> device;
        device.Attach(raw_devices[i]); // adopt MFEnumDeviceSources' reference
        out.push_back(std::move(device));
    }
    ::CoTaskMemFree(raw_devices);
    return S_OK;
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
        if (SUCCEEDED(enumerate_devices(devices))) {
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
        // Raw-pointer assignment AddRefs. An extra explicit AddRef leaks a
        // sample (and potentially its multi-megabyte frame) on every read.
        sample_ = sample;
        grab_cv_.notify_all();
    }

    bool open(int index) {
        std::unique_lock lock(mutex_);
        permission_or_busy_ = false;
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

    bool last_open_was_permission_or_busy() const {
        return permission_or_busy_.load();
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

    // These checks keep each COM failure attributable without logging device
    // identifiers, images, or account information.
    bool open_step(std::string_view step, HRESULT hr) {
        if (SUCCEEDED(hr)) {
            return true;
        }
        log_open_failure(step, hr);
        permission_or_busy_ = permission_or_busy_ || permission_or_busy(hr);
        return false;
    }

    void fail_open() {
        close_worker();
        finish_open(false);
    }

    bool create_reader(bool video_processing) {
        reader_.Reset();
        ComPtr<IMFAttributes> attributes;
        return open_step("create_reader_attributes", ::MFCreateAttributes(&attributes, 3))
            && open_step("set_async_callback", attributes->SetUnknown(
                MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get()))
            // Releasing the native reader must not shut down the source
            // before the video-processing retry. close_worker owns shutdown.
            && open_step("set_source_lifetime", attributes->SetUINT32(
                MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE))
            && open_step("set_video_processing", attributes->SetUINT32(
                MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, video_processing ? TRUE : FALSE))
            && open_step("create_source_reader", ::MFCreateSourceReaderFromMediaSource(
                source_.Get(), attributes.Get(), &reader_))
            && open_step("deselect_streams", reader_->SetStreamSelection(
                MF_SOURCE_READER_ALL_STREAMS, FALSE))
            && open_step("select_video_stream", reader_->SetStreamSelection(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE));
    }

    struct Candidate {
        detail::CaptureMode mode;
        ComPtr<IMFMediaType> type;
    };

    std::vector<Candidate> native_candidates() {
        auto candidates = std::vector<Candidate>{};
        DWORD count = 0;
        for (;; ++count) {
            ComPtr<IMFMediaType> type;
            const auto hr = reader_->GetNativeMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, count, &type);
            if (hr == MF_E_NO_MORE_TYPES) {
                break;
            }
            if (!open_step("get_native_media_type", hr)) {
                break;
            }
            GUID subtype{};
            UINT64 size = 0;
            UINT64 rate = 0;
            const auto subtype_ok = open_step("native_subtype", type->GetGUID(MF_MT_SUBTYPE, &subtype));
            const auto size_ok = open_step("native_size", type->GetUINT64(MF_MT_FRAME_SIZE, &size));
            (void)open_step("native_frame_rate", type->GetUINT64(MF_MT_FRAME_RATE, &rate));
            su::log::process_logger().info("camera.native_type", {
                su::log::field("index", count), su::log::field("subtype", subtype_fourcc(subtype)),
                su::log::field("subtype_data1", subtype.Data1),
                su::log::field("width", static_cast<unsigned>(size >> 32)),
                su::log::field("height", static_cast<unsigned>(size)),
                su::log::field("fps_numerator", static_cast<unsigned>(rate >> 32)),
                su::log::field("fps_denominator", static_cast<unsigned>(rate))});
            if (!subtype_ok || !size_ok || !is_supported_subtype(subtype)) {
                continue;
            }
            const auto format = subtype == MFVideoFormat_MJPG ? detail::CaptureFormat::mjpeg
                : subtype == MFVideoFormat_YUY2 ? detail::CaptureFormat::yuy2
                : detail::CaptureFormat::nv12;
            const auto mode = detail::CaptureMode{
                static_cast<std::uint32_t>(size >> 32), static_cast<std::uint32_t>(size),
                static_cast<std::uint32_t>(rate >> 32), static_cast<std::uint32_t>(rate), format};
            if (mode.valid()) {
                candidates.push_back({mode, std::move(type)});
            } else {
                (void)open_step("invalid_native_size", MF_E_INVALIDMEDIATYPE);
            }
        }
        su::log::process_logger().info("camera.native_types", {
            su::log::field("count", count), su::log::field("supported", candidates.size())});
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return detail::capture_mode_rank(a.mode) < detail::capture_mode_rank(b.mode);
        });
        return candidates;
    }

    bool set_native_type() {
        for (const auto& candidate : native_candidates()) {
            if (open_step("set_native_media_type", reader_->SetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, candidate.type.Get()))) {
                return true;
            }
        }
        return false;
    }

    bool set_processed_type() {
        // Some drivers expose only formats libyuv cannot decode, or incomplete
        // native descriptors. Let MF convert to YUY2, always requesting <=720p.
        for (const auto& size : {std::pair{1280U, 720U}, std::pair{640U, 480U},
                                std::pair{320U, 240U}}) {
            ComPtr<IMFMediaType> requested;
            if (open_step("create_processed_type", ::MFCreateMediaType(&requested))
                && open_step("processed_major_type", requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
                && open_step("processed_subtype", requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2))
                && open_step("processed_size", requested->SetUINT64(MF_MT_FRAME_SIZE,
                    (static_cast<UINT64>(size.first) << 32) | size.second))
                && open_step("set_processed_media_type", reader_->SetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, requested.Get()))) {
                return true;
            }
        }
        return false;
    }

    bool read_negotiated_type(bool video_processing) {
        ComPtr<IMFMediaType> type;
        if (!open_step("get_current_media_type", reader_->GetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type))) {
            return false;
        }
        GUID major{};
        GUID subtype{};
        UINT64 size = 0;
        if (!open_step("negotiated_major_type", type->GetGUID(MF_MT_MAJOR_TYPE, &major))
            || !open_step("negotiated_subtype", type->GetGUID(MF_MT_SUBTYPE, &subtype))
            || !open_step("negotiated_size", type->GetUINT64(MF_MT_FRAME_SIZE, &size))) {
            return false;
        }
        const auto width = static_cast<UINT32>(size >> 32);
        const auto height = static_cast<UINT32>(size);
        const auto format = subtype == MFVideoFormat_MJPG ? detail::CaptureFormat::mjpeg
            : subtype == MFVideoFormat_YUY2 ? detail::CaptureFormat::yuy2 : detail::CaptureFormat::nv12;
        if (major != MFMediaType_Video || !is_supported_subtype(subtype)
            || !detail::CaptureMode{width, height, 0, 0, format}.valid()
            || width > static_cast<UINT32>(std::numeric_limits<int>::max() / 4)
            || height > static_cast<UINT32>(std::numeric_limits<int>::max())
            || (video_processing && (width > 1280 || height > 720))) {
            return open_step("invalid_negotiated_type", MF_E_INVALIDMEDIATYPE);
        }
        UINT32 raw_stride = 0;
        LONG stride = 0;
        const auto stride_hr = type->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride);
        if (SUCCEEDED(stride_hr)) {
            stride = static_cast<INT32>(raw_stride);
        } else if (subtype != MFVideoFormat_MJPG) {
            (void)open_step("get_default_stride", stride_hr);
            if (!open_step("default_bitmap_stride", ::MFGetStrideForBitmapInfoHeader(
                    subtype.Data1, width, &stride))) {
                return false;
            }
        }
        width_ = width;
        height_ = height;
        stride_ = static_cast<std::size_t>(stride < 0 ? -static_cast<std::int64_t>(stride) : stride);
        v4l2_format_ = v4l2_format_for(subtype);
        // MF YUV is top-down. Only retain the existing packed YUY2 negative
        // stride workaround; reversing NV12 rows would corrupt its UV plane.
        bottom_up_ = stride < 0 && v4l2_format_ == kV4l2Yuyv;
        const auto minimum_stride = static_cast<std::size_t>(width) * (v4l2_format_ == kV4l2Yuyv ? 2 : 1);
        if (subtype != MFVideoFormat_MJPG && (stride_ < minimum_stride
            || stride_ > std::numeric_limits<std::size_t>::max() / (static_cast<std::size_t>(height) * 2))) {
            return open_step("invalid_negotiated_stride", MF_E_INVALIDMEDIATYPE);
        }
        UINT64 rate = 0;
        (void)open_step("negotiated_frame_rate", type->GetUINT64(MF_MT_FRAME_RATE, &rate));
        su::log::process_logger().info("camera.mode", {
            su::log::field("format", subtype_fourcc(subtype)),
            su::log::field("width", width_), su::log::field("height", height_),
            su::log::field("stride", stride_),
            su::log::field("path", video_processing ? "video_processing" : "native"),
            su::log::field("fps_numerator", static_cast<unsigned>(rate >> 32)),
            su::log::field("fps_denominator", static_cast<unsigned>(rate))});
        return true;
    }

    void open_worker(int index) {
        close_worker();
        permission_or_busy_ = false;
        std::vector<ComPtr<IMFActivate>> devices;
        if (!open_step("enumerate_devices", enumerate_devices(devices))) {
            fail_open();
            return;
        }
        if (index < 0 || static_cast<std::size_t>(index) >= devices.size()) {
            (void)open_step("camera_index", E_INVALIDARG);
            fail_open();
            return;
        }
        if (!open_step("activate_source", devices[static_cast<std::size_t>(index)]->ActivateObject(
                IID_PPV_ARGS(&source_)))) {
            fail_open();
            return;
        }
        callback_.Attach(new StreamCallback(static_cast<MfGrabSink*>(this)));
        if (!create_reader(false)) {
            fail_open();
            return;
        }
        auto video_processing = false;
        if (!set_native_type()) {
            video_processing = true;
            su::log::process_logger().info("camera.open_path", {
                su::log::field("path", "video_processing"),
                su::log::field("reason", "no_usable_native_type")});
            if (!create_reader(true) || !set_processed_type()) {
                fail_open();
                return;
            }
        }
        if (!read_negotiated_type(video_processing)) {
            fail_open();
            return;
        }
        permission_or_busy_ = false;
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
            auto frame_stride = stride_;
            ComPtr<IMF2DBuffer> surface;
            if (v4l2_format_ == kV4l2Nv12 && SUCCEEDED(buffer.As(&surface))) {
                // Lock() may expose a hardware surface with extra rows or a
                // different pitch from MF_MT_DEFAULT_STRIDE. MF's contiguous
                // copy removes that padding and places UV after width*height.
                DWORD length = 0;
                const auto y_size = static_cast<std::size_t>(width_) * height_;
                const auto required = y_size + y_size / 2;
                if (FAILED(surface->GetContiguousLength(&length)) || length < required) {
                    finish_grab(false);
                    return;
                }
                grab_buffer_.resize(length);
                if (FAILED(surface->ContiguousCopyTo(grab_buffer_.data(), length))) {
                    finish_grab(false);
                    return;
                }
                frame_stride = width_;
            } else if (!copy_locked_buffer(buffer.Get())) {
                finish_grab(false);
                return;
            }
            frame_.width = static_cast<int>(width_);
            frame_.height = static_cast<int>(height_);
            frame_.stride = frame_stride;
            frame_.v4l2_format = v4l2_format_;
            frame_.bytes = grab_buffer_.data();
            frame_.byte_count = grab_buffer_.size();
            grab_result_ = true;
            break;
        }
        finish_grab(grab_result_);
    }

    bool copy_locked_buffer(IMFMediaBuffer* buffer) {
        BYTE* data = nullptr;
        DWORD current = 0;
        DWORD max = 0;
        if (FAILED(buffer->Lock(&data, &max, &current))) {
            return false;
        }
        if (data == nullptr) {
            buffer->Unlock();
            return false;
        }
        const auto image_bytes = stride_ * static_cast<std::size_t>(height_);
        const auto required = v4l2_format_ == kV4l2Nv12 ? image_bytes + image_bytes / 2
            : v4l2_format_ == kV4l2Yuyv ? image_bytes : 1;
        if (current < required || current > max) {
            buffer->Unlock();
            return false;
        }
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
        return true;
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
        if (source_) {
            (void)source_->Shutdown();
            source_.Reset();
        }
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
    std::atomic<bool> permission_or_busy_{false};
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
    ComPtr<IMFMediaSource> source_;
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

bool MfCameraStream::last_open_was_permission_or_busy() const {
    return impl_->last_open_was_permission_or_busy();
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
