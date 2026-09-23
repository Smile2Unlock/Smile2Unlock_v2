#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace su::windows::auth_service {

// Runs accepted pipe transactions on a bounded set of worker threads so a
// recognition attempt of up to ~30 s cannot block settings writes, manual
// requests or other sessions. submit() never blocks: when every worker is
// busy and the pending queue is full it returns false and the caller rejects
// the request itself. Tasks must remain copyable (std::function); pipe
// ownership is therefore passed as a raw handle parameter, not captured.
// Stopping drains: accepted transactions must run to completion because each
// task owns its client's pipe handle — dropping one would leak the handle and
// leave the client waiting. Tasks observe the service stop event and finish
// promptly, so the drain cannot wedge shutdown.
class RequestWorkerPool {
public:
    using Task = std::function<void()>;

    static constexpr std::size_t kDefaultWorkers = 4;
    static constexpr std::size_t kDefaultQueueCapacity = 8;

    RequestWorkerPool()
        : RequestWorkerPool(kDefaultWorkers, kDefaultQueueCapacity) {}
    explicit RequestWorkerPool(std::size_t worker_count, std::size_t queue_capacity);
    ~RequestWorkerPool();
    RequestWorkerPool(const RequestWorkerPool&) = delete;
    RequestWorkerPool& operator=(const RequestWorkerPool&) = delete;

    // Returns false when the pool is stopping or the queue is full.
    [[nodiscard]] bool submit(Task task);

private:
    void run();

    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::deque<Task> queue_;
    std::vector<std::thread> workers_;
    std::size_t capacity_;
    bool stopping_ = false;
};

inline RequestWorkerPool::RequestWorkerPool(
    std::size_t worker_count, std::size_t queue_capacity)
    : capacity_{queue_capacity == 0 ? 1 : queue_capacity} {
    if (worker_count == 0) {
        worker_count = 1;
    }
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers_.emplace_back([this] { run(); });
    }
}

// Stopping makes the workers drain the queue and exit; tasks already accepted
// still run so their pipe handles are closed by the task itself.
inline RequestWorkerPool::~RequestWorkerPool() {
    {
        const auto lock = std::lock_guard{mutex_};
        stopping_ = true;
    }
    work_ready_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }
}

inline bool RequestWorkerPool::submit(Task task) {
    {
        const auto lock = std::lock_guard{mutex_};
        if (stopping_ || queue_.size() >= capacity_) {
            return false;
        }
        queue_.push_back(std::move(task));
    }
    work_ready_.notify_one();
    return true;
}

inline void RequestWorkerPool::run() {
    while (true) {
        auto task = Task{};
        {
            auto lock = std::unique_lock{mutex_};
            work_ready_.wait(
                lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                // stopping_ with an empty queue: drain complete.
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

} // namespace su::windows::auth_service
