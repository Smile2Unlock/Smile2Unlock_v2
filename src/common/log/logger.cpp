#include "logger.h"
#include <ctime>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace su::log {
namespace {
Sink emergency_sink() {
#ifdef _WIN32
    return debugger_sink();
#else
    return stderr_sink();
#endif
}

int utc_offset(Timestamp now) noexcept {
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    auto local = std::tm{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
    auto utc = std::tm{};
    gmtime_s(&utc, &seconds);
    return static_cast<int>((_mkgmtime(&local) - _mkgmtime(&utc)) / 60);
#else
    localtime_r(&seconds, &local);
    return static_cast<int>(local.tm_gmtoff / 60);
#endif
}
std::uint64_t process_id() noexcept {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}
std::uint64_t thread_id() noexcept {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return syscall(SYS_gettid);
#endif
}
} // namespace

Logger::Logger(std::string component, Level minimum, Sink sink, std::vector<Field> context,
               Clock clock)
    : component_(std::move(component)), minimum_(minimum),
      sink_(fallback(std::move(sink), emergency_sink())), context_(std::move(context)),
      clock_(clock ? std::move(clock) : Clock{[] {
          return std::chrono::time_point_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now());
      }}) {}

Logger Logger::with(std::initializer_list<Field> fields) const {
    auto context = context_;
    context.insert(context.end(), fields.begin(), fields.end());
    return Logger{component_, minimum_, sink_, std::move(context), clock_};
}

void Logger::write(Level level, std::string_view event, std::vector<Field> fields,
                   std::source_location source) const noexcept {
    if (!enabled(minimum_, level))
        return;
    try {
        auto context = context_;
        context.insert(context.end(), std::make_move_iterator(fields.begin()),
                       std::make_move_iterator(fields.end()));
        const auto now = clock_();
        const auto line =
            format_record({now, utc_offset(now), level, component_, std::string{event},
                           process_id(), thread_id(), std::move(context), source});
        try {
            (void)sink_(level, line);
        } catch (...) {
        }
    } catch (...) {
    }
}
} // namespace su::log
