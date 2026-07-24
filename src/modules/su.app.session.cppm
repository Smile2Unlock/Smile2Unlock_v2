module;

#if defined(__linux__)
#include <systemd/sd-bus.h>
#include <unistd.h>
#endif

export module su.app.session;

import std;

export namespace su::app {

class SessionLockMonitor {
public:
    using LockCallback = std::move_only_function<void()>;

    explicit SessionLockMonitor(LockCallback callback);
    ~SessionLockMonitor();
    SessionLockMonitor(const SessionLockMonitor&) = delete;
    SessionLockMonitor& operator=(const SessionLockMonitor&) = delete;
    SessionLockMonitor(SessionLockMonitor&&) = delete;
    SessionLockMonitor& operator=(SessionLockMonitor&&) = delete;

    [[nodiscard]] bool available() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace su::app

namespace su::app {

#if defined(__linux__)

namespace {

std::expected<std::string, std::string> read_object_path(sd_bus_message* reply) {
    const char* path = nullptr;
    if (::sd_bus_message_read(reply, "o", &path) < 0 || path == nullptr || path[0] == '\0') {
        return std::unexpected("logind returned an invalid session path");
    }
    return std::string(path);
}

std::expected<std::string, std::string> session_path_for_process(sd_bus* bus) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    auto result = ::sd_bus_call_method(
        bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "GetSessionByPID",
        &error,
        &reply,
        "u",
        static_cast<std::uint32_t>(::getpid()));
    if (result >= 0) {
        auto path = read_object_path(reply);
        ::sd_bus_message_unref(reply);
        ::sd_bus_error_free(&error);
        return path;
    }
    ::sd_bus_message_unref(reply);
    ::sd_bus_error_free(&error);

    const auto* session_id = std::getenv("XDG_SESSION_ID");
    if (session_id == nullptr || session_id[0] == '\0') {
        return std::unexpected("current process is not associated with a logind session");
    }

    error = SD_BUS_ERROR_NULL;
    reply = nullptr;
    result = ::sd_bus_call_method(
        bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "GetSession",
        &error,
        &reply,
        "s",
        session_id);
    if (result < 0) {
        const auto message = error.message != nullptr
            ? std::string(error.message)
            : std::string("failed to resolve the current logind session");
        ::sd_bus_message_unref(reply);
        ::sd_bus_error_free(&error);
        return std::unexpected(message);
    }
    auto path = read_object_path(reply);
    ::sd_bus_message_unref(reply);
    ::sd_bus_error_free(&error);
    return path;
}

} // namespace

class SessionLockMonitor::Impl {
public:
    explicit Impl(LockCallback callback)
        : callback_(std::move(callback)), thread_([this] { run(); }) {}

    ~Impl() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] bool available() const {
        return available_.load(std::memory_order_acquire);
    }

private:
    void notify_lock() {
        if (running_.load(std::memory_order_acquire) && callback_) {
            callback_();
        }
    }

    static int on_lock(sd_bus_message*, void* userdata, sd_bus_error*) {
        auto* self = static_cast<Impl*>(userdata);
        self->notify_lock();
        return 0;
    }

    static int on_prepare_for_sleep(sd_bus_message* message, void* userdata, sd_bus_error*) {
        auto going_to_sleep = 0;
        if (::sd_bus_message_read(message, "b", &going_to_sleep) >= 0 && going_to_sleep != 0) {
            static_cast<Impl*>(userdata)->notify_lock();
        }
        return 0;
    }

    static int on_properties_changed(sd_bus_message* message, void* userdata, sd_bus_error*) {
        const char* interface = nullptr;
        if (::sd_bus_message_read_basic(message, 's', &interface) < 0
            || interface == nullptr
            || std::string_view(interface) != "org.freedesktop.login1.Session"
            || ::sd_bus_message_enter_container(message, 'a', "{sv}") < 0) {
            return 0;
        }
        while (::sd_bus_message_enter_container(message, 'e', "sv") > 0) {
            const char* property = nullptr;
            if (::sd_bus_message_read_basic(message, 's', &property) < 0
                || ::sd_bus_message_enter_container(message, 'v', nullptr) < 0) {
                (void)::sd_bus_message_exit_container(message);
                continue;
            }
            if (property != nullptr && std::string_view(property) == "LockedHint") {
                auto locked = 0;
                if (::sd_bus_message_read_basic(message, 'b', &locked) >= 0 && locked != 0) {
                    static_cast<Impl*>(userdata)->notify_lock();
                }
            } else {
                (void)::sd_bus_message_skip(message, nullptr);
            }
            (void)::sd_bus_message_exit_container(message);
            (void)::sd_bus_message_exit_container(message);
        }
        (void)::sd_bus_message_exit_container(message);
        return 0;
    }

    void run() {
        sd_bus* bus = nullptr;
        sd_bus_slot* lock_slot = nullptr;
        sd_bus_slot* properties_slot = nullptr;
        sd_bus_slot* sleep_slot = nullptr;
        if (::sd_bus_open_system(&bus) < 0) {
            std::println(stderr, "[session] failed to connect to the system bus");
            return;
        }

        const auto session_path = session_path_for_process(bus);
        if (!session_path) {
            std::println(stderr, "[session] lock monitoring unavailable: {}", session_path.error());
            ::sd_bus_unref(bus);
            return;
        }
        if (::sd_bus_match_signal(
                bus,
                &lock_slot,
                "org.freedesktop.login1",
                session_path->c_str(),
                "org.freedesktop.login1.Session",
                "Lock",
                &Impl::on_lock,
                this) < 0) {
            std::println(stderr, "[session] failed to subscribe to logind Lock");
            ::sd_bus_unref(bus);
            return;
        }
        if (::sd_bus_match_signal(
                bus,
                &properties_slot,
                "org.freedesktop.login1",
                session_path->c_str(),
                "org.freedesktop.DBus.Properties",
                "PropertiesChanged",
                &Impl::on_properties_changed,
                this) < 0) {
            std::println(stderr, "[session] failed to subscribe to logind LockedHint");
            ::sd_bus_slot_unref(lock_slot);
            ::sd_bus_unref(bus);
            return;
        }
        if (::sd_bus_match_signal(
                bus,
                &sleep_slot,
                "org.freedesktop.login1",
                "/org/freedesktop/login1",
                "org.freedesktop.login1.Manager",
                "PrepareForSleep",
                &Impl::on_prepare_for_sleep,
                this) < 0) {
            std::println(stderr, "[session] failed to subscribe to logind PrepareForSleep");
            ::sd_bus_slot_unref(properties_slot);
            ::sd_bus_slot_unref(lock_slot);
            ::sd_bus_unref(bus);
            return;
        }

        available_.store(true, std::memory_order_release);
        std::println(stderr, "[session] monitoring logind Lock on {}", *session_path);
        while (running_.load(std::memory_order_acquire)) {
            const auto processed = ::sd_bus_process(bus, nullptr);
            if (processed < 0) {
                break;
            }
            if (processed == 0) {
                (void)::sd_bus_wait(bus, 250'000);
            }
        }
        available_.store(false, std::memory_order_release);
        ::sd_bus_slot_unref(sleep_slot);
        ::sd_bus_slot_unref(properties_slot);
        ::sd_bus_slot_unref(lock_slot);
        ::sd_bus_unref(bus);
    }

    LockCallback callback_;
    std::atomic<bool> running_{true};
    std::atomic<bool> available_{false};
    std::thread thread_;
};

#else

class SessionLockMonitor::Impl {
public:
    explicit Impl(LockCallback) {}
    [[nodiscard]] bool available() const { return false; }
};

#endif

SessionLockMonitor::SessionLockMonitor(LockCallback callback)
    : impl_(std::make_unique<Impl>(std::move(callback))) {}

SessionLockMonitor::~SessionLockMonitor() = default;

bool SessionLockMonitor::available() const {
    return impl_->available();
}

} // namespace su::app
