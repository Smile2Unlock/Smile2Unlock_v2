#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace su::capture {
// Stop-aware maintenance runs even when no user opens the viewer or unlocks.
class Maintenance {
public:
    explicit Maintenance(std::function<void()> sweep)
        : worker_([sweep = std::move(sweep)](std::stop_token stop) {
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(mutex);
            while (!stop.stop_requested()) {
                try { sweep(); } catch (...) { /* best effort */ }
                wake.wait_for(lock, stop, std::chrono::minutes{1}, [] { return false; });
            }
        }) {}
private:
    std::jthread worker_;
};
}
