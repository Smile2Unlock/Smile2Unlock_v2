#include "sinks.h"
#include <cstdio>
#include <mutex>
#ifndef _WIN32
#include <syslog.h>
#endif

namespace su::log {
namespace {
std::expected<void, SinkError> invoke(const Sink& sink, Level level,
                                      std::string_view line) noexcept {
    try {
        return sink(level, line);
    } catch (...) {
        return std::unexpected(SinkError::write_failed);
    }
}
} // namespace
Sink stderr_sink() {
    return [](Level, std::string_view line) -> std::expected<void, SinkError> {
        // fwrite serializes a complete record on the C stream, including its newline.
        if (std::fwrite(line.data(), 1, line.size(), stderr) != line.size())
            return std::unexpected(SinkError::write_failed);
        return {};
    };
}
Sink fallback(Sink primary, Sink secondary) {
    return [primary = std::move(primary), secondary = std::move(secondary)](Level level,
                                                                            std::string_view line) {
        const auto result = invoke(primary, level, line);
        return result ? result : invoke(secondary, level, line);
    };
}
Sink tee(Sink first, Sink second) {
    return
        [first = std::move(first), second = std::move(second)](Level level, std::string_view line) {
            const auto a = invoke(first, level, line);
            const auto b = invoke(second, level, line);
            return a ? b : a;
        };
}
std::vector<Field> to_fields(SinkError error) {
    return {field("sink_error", static_cast<int>(error))};
}
#ifndef _WIN32
Sink syslog_sink(std::string ident, int facility) {
    return [ident = std::move(ident),
            facility](Level level, std::string_view line) -> std::expected<void, SinkError> {
        const auto priority = level == Level::debug  ? LOG_DEBUG
                              : level == Level::info ? LOG_INFO
                              : level == Level::warn ? LOG_WARNING
                                                     : LOG_ERR;
        // Do not use openlog: its ident pointer and configuration are process-global.
        syslog(facility | priority, "%s: %.*s", ident.c_str(), static_cast<int>(line.size()),
               line.data());
        return {};
    };
}
#endif
} // namespace su::log
