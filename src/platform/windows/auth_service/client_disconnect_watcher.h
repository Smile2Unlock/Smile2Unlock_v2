#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <memory>

namespace su::windows::auth_service {

namespace detail {

struct HandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(handle));
        }
    }
};

using WatcherEvent = std::unique_ptr<void, HandleCloser>;

} // namespace detail

// Watches for the pipe client closing its handle while its transaction runs
// on a pool worker. The protocol is one message per connection, so a pending
// overlapped ReadFile stays queued until the client disconnects and completes
// with ERROR_BROKEN_PIPE. Any earlier completion (unexpected data, protocol
// violation) also signals, which cancels the transaction's recognition
// agent. This is what turns a client-side cancel into "terminate the camera
// agent now" instead of letting it run out its ~30 s budget.
class ClientDisconnectWatcher {
public:
    explicit ClientDisconnectWatcher(HANDLE pipe) : pipe_{pipe} {
        const auto raw_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (raw_event == nullptr) {
            return;
        }
        event_.reset(raw_event);
        overlapped_.hEvent = raw_event;
        // A scratch read smaller than any legal second message; there must
        // not be a second message at all.
        if (!ReadFile(
                pipe_, scratch_.data(), static_cast<DWORD>(scratch_.size()), &bytes_,
                &overlapped_)) {
            const auto error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                started_ = true;
            } else {
                // Client is already gone (e.g. ERROR_BROKEN_PIPE) or the read
                // failed: signal so the transaction aborts immediately.
                (void)SetEvent(raw_event);
                completed_ = true;
                started_ = true;
            }
        } else {
            // Unexpected data arrived: protocol violation, treat as gone.
            (void)SetEvent(raw_event);
            completed_ = true;
            started_ = true;
        }
    }

    ~ClientDisconnectWatcher() { finish(); }
    ClientDisconnectWatcher(const ClientDisconnectWatcher&) = delete;
    ClientDisconnectWatcher& operator=(const ClientDisconnectWatcher&) = delete;

    // Event that is signaled when the client is (or may be) gone. Null when
    // the watcher could not start; callers then skip disconnect tracking.
    [[nodiscard]] HANDLE event() const {
        return started_ ? overlapped_.hEvent : nullptr;
    }

    // Reclaim the pending OVERLAPPED before the handle is reused for the
    // response write or closed.
    void finish() {
        if (!started_ || finished_) {
            return;
        }
        finished_ = true;
        if (!completed_) {
            (void)CancelIoEx(pipe_, &overlapped_);
        }
        auto transferred = DWORD{0};
        (void)GetOverlappedResult(pipe_, &overlapped_, &transferred, TRUE);
    }

private:
    HANDLE pipe_;
    detail::WatcherEvent event_{};
    OVERLAPPED overlapped_{};
    std::array<std::byte, 16> scratch_{};
    DWORD bytes_ = 0;
    bool started_ = false;
    bool completed_ = false;
    bool finished_ = false;
};

} // namespace su::windows::auth_service
