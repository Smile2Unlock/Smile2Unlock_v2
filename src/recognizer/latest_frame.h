#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace su::recognizer::detail {

// One pending frame: capture never waits for inference, and a slow consumer
// processes the newest frame rather than accumulating seconds of latency.
template <class Frame>
class LatestFrameQueue {
public:
    void publish(std::shared_ptr<const Frame> frame) {
        std::lock_guard lock(mutex_);
        if (!closed_) {
            latest_ = std::move(frame);
            ++sequence_;
            cv_.notify_one();
        }
    }

    std::shared_ptr<const Frame> wait_next(std::uint64_t& consumed) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return closed_ || sequence_ != consumed; });
        if (closed_) {
            return {};
        }
        consumed = sequence_;
        return latest_;
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        latest_.reset();
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<const Frame> latest_;
    std::uint64_t sequence_ = 0;
    bool closed_ = false;
};

} // namespace su::recognizer::detail
