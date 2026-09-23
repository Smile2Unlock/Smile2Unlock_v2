#include "request_worker_pool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

using Pool = su::windows::auth_service::RequestWorkerPool;

// Blocks every worker until release() so the queue fills deterministically.
class ReleaseGate {
public:
    explicit ReleaseGate(
        std::chrono::milliseconds budget = std::chrono::seconds{30})
        : budget_{budget} {}

    void wait() {
        auto lock = std::unique_lock{mutex_};
        cv_.wait_for(lock, budget_, [this] { return released_; });
    }
    void release() {
        {
            const auto lock = std::lock_guard{mutex_};
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    std::chrono::milliseconds budget_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool released_ = false;
};

bool wait_until(const std::function<bool()>& predicate) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return predicate();
}

} // namespace

int main() {
    // Every submitted task runs to completion. The task count stays within
    // workers + queue capacity so acceptance does not depend on how fast the
    // workers drain (bounded rejection has its own case below).
    {
        auto pool = Pool{2, 16};
        auto completed = std::atomic<int>{0};
        for (int index = 0; index < 16; ++index) {
            if (!pool.submit([&completed] { completed.fetch_add(1); })) {
                std::cerr << "submit rejected while the queue had capacity\n";
                return 1;
            }
        }
        if (!wait_until([&completed] { return completed.load() == 16; })) {
            std::cerr << "not all tasks completed\n";
            return 1;
        }
    }

    // Workers actually run side by side: two tasks wait for each other, so
    // the rendezvous only completes promptly when both are running at the
    // same time. entered is never decremented during the wait — a task that
    // observes 1 must not make the other task's condition unreachable.
    // Sequential execution would only set overlapped after the first task's
    // full spin budget, which the elapsed-time assertion rejects.
    {
        auto pool = Pool{2, 2};
        auto entered = std::atomic<int>{0};
        auto overlapped = std::atomic<bool>{false};
        const auto spin_budget = std::chrono::seconds{10};
        const auto spin = [&entered, &overlapped, spin_budget] {
            entered.fetch_add(1);
            const auto deadline =
                std::chrono::steady_clock::now() + spin_budget;
            while (std::chrono::steady_clock::now() < deadline) {
                if (entered.load() >= 2) {
                    overlapped.store(true);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        };
        const auto started = std::chrono::steady_clock::now();
        if (!pool.submit(spin) || !pool.submit(spin)) {
            std::cerr << "parallel probe tasks were rejected\n";
            return 1;
        }
        if (!wait_until([&entered] { return entered.load() == 2; })
            || !overlapped.load()) {
            std::cerr << "two workers never rendezvoused concurrently\n";
            return 1;
        }
        if (std::chrono::steady_clock::now() - started >= spin_budget) {
            std::cerr << "rendezvous only completed after the full spin budget\n";
            return 1;
        }
    }

    // The queue is bounded: with both workers blocked and the queue full, the
    // next submit is rejected instead of blocking or growing the pool. Waiting
    // for the blockers to start first makes "queue full" deterministic.
    {
        auto pool = Pool{2, 2};
        auto gate = ReleaseGate{};
        auto entered = std::atomic<int>{0};
        auto finished = std::atomic<int>{0};
        const auto blocker = [&gate, &entered, &finished] {
            entered.fetch_add(1);
            gate.wait();
            finished.fetch_add(1);
        };
        if (!pool.submit(blocker) || !pool.submit(blocker)) {
            std::cerr << "running blocker was rejected\n";
            return 1;
        }
        if (!wait_until([&entered] { return entered.load() == 2; })) {
            std::cerr << "blockers never started\n";
            return 1;
        }
        if (!pool.submit(blocker) || !pool.submit(blocker)) {
            std::cerr << "queued task rejected while capacity remained\n";
            return 1;
        }
        if (pool.submit(blocker)) {
            std::cerr << "submit accepted past the bounded queue capacity\n";
            return 1;
        }
        gate.release();
        if (!wait_until([&finished] { return finished.load() == 4; })) {
            std::cerr << "blocked and queued tasks did not drain\n";
            return 1;
        }
    }

    // Saturation is transient: after the blockers drain, submit works again.
    {
        auto pool = Pool{1, 1};
        auto gate = ReleaseGate{};
        auto entered = std::atomic<int>{0};
        auto finished = std::atomic<int>{0};
        const auto blocker = [&] {
            entered.fetch_add(1);
            gate.wait();
            finished.fetch_add(1);
        };
        if (!pool.submit(blocker)) {
            std::cerr << "saturation setup task was rejected\n";
            return 1;
        }
        if (!wait_until([&entered] { return entered.load() == 1; })) {
            std::cerr << "blocker never started\n";
            return 1;
        }
        if (!pool.submit(blocker)) {
            std::cerr << "queued saturation task was rejected\n";
            return 1;
        }
        if (pool.submit([] {})) {
            std::cerr << "single worker plus full queue accepted more work\n";
            return 1;
        }
        gate.release();
        if (!wait_until([&finished] { return finished.load() == 2; })) {
            std::cerr << "saturation setup did not drain\n";
            return 1;
        }
        auto recovered = std::atomic<bool>{false};
        if (!pool.submit([&recovered] { recovered.store(true); })) {
            std::cerr << "submit did not recover after saturation\n";
            return 1;
        }
        if (!wait_until([&recovered] { return recovered.load(); })) {
            std::cerr << "post-saturation task never ran\n";
            return 1;
        }
    }

    // Stopping drains accepted work instead of dropping it: a queued task
    // still runs during pool destruction (pipe-owning tasks must close their
    // own handles). The blocker is bounded by a short gate budget so the
    // destructor join returns promptly in the test.
    {
        auto gate = ReleaseGate{std::chrono::milliseconds{500}};
        auto drained = std::atomic<bool>{false};
        {
            auto pool = Pool{1, 2};
            if (!pool.submit([&] { gate.wait(); })) {
                std::cerr << "drain blocker was rejected\n";
                return 1;
            }
            if (!pool.submit([&] { drained.store(true); })) {
                std::cerr << "drain marker was rejected\n";
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        } // destructor joins: blocker rides out its gate budget, then the
          // queued marker must run before the join returns.
        if (!drained.load()) {
            std::cerr << "queued task was dropped on shutdown\n";
            return 1;
        }
    }

    // Destruction joins idle workers promptly; the destructor running after
    // this scope is the assertion (a hang fails the test timeout).
    std::cout << "request worker pool: ok\n";
    return 0;
}
