module;


#include <windows.h>


export module service_runtime;

import std;

export namespace smile2unlock {

class ServiceRuntime {
public:
    ServiceRuntime();
    ~ServiceRuntime();

    void Run();
    void Stop();

private:
    struct DetachedTask {
        struct promise_type {
            DetachedTask get_return_object() noexcept { return {}; }
            std::suspend_never initial_suspend() noexcept { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }
            void return_void() noexcept {}
            void unhandled_exception() noexcept { std::terminate(); }
        };
    };

    struct SleepAwaiter {
        DWORD duration_ms;

        bool await_ready() const noexcept { return duration_ms == 0; }

        void await_suspend(std::coroutine_handle<> handle) const {
            std::thread([duration_ms = duration_ms, handle]() mutable {
                ::Sleep(duration_ms);
                handle.resume();
            }).detach();
        }

        void await_resume() const noexcept {}
    };

    DetachedTask idle_loop();

    HANDLE shutdown_event_;
    std::atomic<bool> running_{true};
};

} // namespace smile2unlock

namespace smile2unlock {

ServiceRuntime::ServiceRuntime()
    : shutdown_event_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
}

ServiceRuntime::~ServiceRuntime() {
    if (shutdown_event_ != nullptr) {
        ::CloseHandle(shutdown_event_);
        shutdown_event_ = nullptr;
    }
}

ServiceRuntime::DetachedTask ServiceRuntime::idle_loop() {
    while (running_.load()) {
        co_await SleepAwaiter{1000};
    }
}

void ServiceRuntime::Run() {
    idle_loop();

    if (shutdown_event_ != nullptr) {
        ::WaitForSingleObject(shutdown_event_, INFINITE);
    }
}

void ServiceRuntime::Stop() {
    running_.store(false);
    if (shutdown_event_ != nullptr) {
        ::SetEvent(shutdown_event_);
    }
}

} // namespace smile2unlock
