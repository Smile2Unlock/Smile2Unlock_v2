#pragma once
// Turns SIGTERM/SIGINT into an ordinary, pollable event (Linux).
//
// A process killed by an unhandled signal never runs destructors, so camera
// controls changed during authentication (exposure, backlight compensation)
// stay in the camera's firmware. Installing this lets the process shut down
// through its normal path instead.
//
// The handler is async-signal-safe: it only sets a flag and writes one byte
// to a pipe. The signal mask is left alone, so child processes started later
// keep their default signal behaviour (a blocked mask would survive exec).
// SIGKILL, crashes and power loss cannot be caught; nothing restores then.

#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include <atomic>
#include <cerrno>

namespace su::process {

namespace detail {

inline std::atomic<int> shutdown_write_fd{-1};
inline std::atomic<bool> shutdown_requested{false};

inline void on_shutdown_signal(int) {
    const auto saved_errno = errno;
    shutdown_requested.store(true, std::memory_order_relaxed);
    const auto fd = shutdown_write_fd.load(std::memory_order_relaxed);
    if (fd >= 0) {
        const char byte = 1;
        // A full pipe already means "signalled"; the result is irrelevant.
        [[maybe_unused]] const auto written = ::write(fd, &byte, 1);
    }
    errno = saved_errno;
}

}  // namespace detail

// One per process. Not movable: the handler refers to its pipe.
class ShutdownSignal {
public:
    ShutdownSignal() {
        int fds[2] = {-1, -1};
        if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
            return;
        }
        read_fd_ = fds[0];
        write_fd_ = fds[1];
        detail::shutdown_requested.store(false);
        detail::shutdown_write_fd.store(write_fd_);
        struct sigaction action{};
        action.sa_handler = &detail::on_shutdown_signal;
        ::sigemptyset(&action.sa_mask);
        installed_ = ::sigaction(SIGTERM, &action, nullptr) == 0
            && ::sigaction(SIGINT, &action, nullptr) == 0;
    }

    ~ShutdownSignal() {
        if (write_fd_ >= 0) {
            ::signal(SIGTERM, SIG_DFL);
            ::signal(SIGINT, SIG_DFL);
            detail::shutdown_write_fd.store(-1);
            ::close(write_fd_);
            ::close(read_fd_);
        }
    }

    ShutdownSignal(const ShutdownSignal&) = delete;
    ShutdownSignal& operator=(const ShutdownSignal&) = delete;

    [[nodiscard]] bool installed() const { return installed_; }

    // Readable once a signal arrived or wake() was called; never drained, so
    // several pollers all see it.
    [[nodiscard]] int pollable_fd() const { return read_fd_; }

    [[nodiscard]] bool requested() const {
        return detail::shutdown_requested.load(std::memory_order_relaxed);
    }

    // Wakes waiters without a signal (tear-down of a watcher thread).
    void wake() const {
        const char byte = 1;
        [[maybe_unused]] const auto written = ::write(write_fd_, &byte, 1);
    }

    // Blocks until a signal arrives or wake() is called.
    void wait() const {
        auto poll_fd = pollfd{.fd = read_fd_, .events = POLLIN, .revents = 0};
        while (::poll(&poll_fd, 1, -1) < 0 && errno == EINTR) {
        }
    }

private:
    int read_fd_ = -1;
    int write_fd_ = -1;
    bool installed_ = false;
};

}  // namespace su::process
