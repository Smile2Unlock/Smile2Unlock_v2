module;


#include <windows.h>
#include "models/gui_ipc_protocol.h"


export module service_runtime;

import std;

export namespace smile2unlock {

void NotifyServiceActivity();

// 服务互斥体 - 用于检测是否有 Service 进程在运行
class ServiceMutex {
public:
    ServiceMutex();
    ~ServiceMutex();

    // 尝试创建互斥体（Service 模式调用）
    bool try_create();

    // 检查是否有其他 Service 进程在运行
    static bool is_service_running();

    void release();

private:
    HANDLE mutex_;
    bool owns_;
};

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

namespace {

std::atomic<ULONGLONG> g_last_activity_tick{0};
//TODO: make this configurable from the config file
constexpr ULONGLONG kServiceInactivityTimeoutMs = 60000;

}

void NotifyServiceActivity() {
    g_last_activity_tick.store(::GetTickCount64(), std::memory_order_relaxed);
}

// ServiceMutex 实现
ServiceMutex::ServiceMutex() : mutex_(nullptr), owns_(false) {}

ServiceMutex::~ServiceMutex() { release(); }

bool ServiceMutex::try_create() {
    mutex_ = CreateMutexA(nullptr, TRUE, SERVICE_MUTEX_NAME);
    if (mutex_ == nullptr) {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return false;
    }
    owns_ = true;
    return true;
}

bool ServiceMutex::is_service_running() {
    HANDLE h = OpenMutexA(SYNCHRONIZE, FALSE, SERVICE_MUTEX_NAME);
    if (h != nullptr) {
        CloseHandle(h);
        return true;
    }
    return GetLastError() != ERROR_FILE_NOT_FOUND;
}

void ServiceMutex::release() {
    if (owns_ && mutex_) {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
        mutex_ = nullptr;
        owns_ = false;
    }
}

// ServiceRuntime 实现

ServiceRuntime::ServiceRuntime()
    : shutdown_event_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    NotifyServiceActivity();
}

ServiceRuntime::~ServiceRuntime() {
    if (shutdown_event_ != nullptr) {
        ::CloseHandle(shutdown_event_);
        shutdown_event_ = nullptr;
    }
}

ServiceRuntime::DetachedTask ServiceRuntime::idle_loop() {
    while (running_.load()) {
        const ULONGLONG last_activity = g_last_activity_tick.load(std::memory_order_relaxed);
        const ULONGLONG now = ::GetTickCount64();
        if (last_activity != 0 && now >= last_activity &&
            now - last_activity >= kServiceInactivityTimeoutMs) {
            Stop();
            co_return;
        }
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
