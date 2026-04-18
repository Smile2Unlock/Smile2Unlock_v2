export module rewrite.task;

import std;

export namespace rewrite::async {

class Task {
public:
    struct promise_type {
        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_never initial_suspend() const noexcept {
            return {};
        }

        std::suspend_always final_suspend() const noexcept {
            return {};
        }

        void return_void() const noexcept {}

        void unhandled_exception() {
            std::terminate();
        }
    };

    explicit Task(std::coroutine_handle<promise_type> handle) noexcept
        : handle_(handle) {}

    Task(Task&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}

    Task(const Task&) = delete;

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    Task& operator=(const Task&) = delete;

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

private:
    std::coroutine_handle<promise_type> handle_{};
};

} // namespace rewrite::async
