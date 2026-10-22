#pragma once
#include "sinks.h"
#include <initializer_list>
#include <utility>

namespace su::log {
using Clock = std::function<Timestamp()>;
class Logger {
public:
    Logger(std::string component, Level minimum, Sink sink, std::vector<Field> context = {},
           Clock clock = {});
    Logger with(std::initializer_list<Field> fields) const;
    void write(Level level, std::string_view event, std::vector<Field> fields = {},
               std::source_location source = std::source_location::current()) const noexcept;
#define SU_LOG_METHOD(name)                                                                        \
    void name(std::string_view event, std::vector<Field> fields = {},                              \
              std::source_location source = std::source_location::current()) const noexcept {      \
        write(Level::name, event, std::move(fields), source);                                      \
    }
    SU_LOG_METHOD(debug)
    SU_LOG_METHOD(info)
    SU_LOG_METHOD(warn)
    SU_LOG_METHOD(error)
#undef SU_LOG_METHOD
private:
    const std::string component_;
    const Level minimum_;
    const Sink sink_;
    const std::vector<Field> context_;
    const Clock clock_;
};

// The explicit result type permits expected::or_else to preserve its value type.
template <typename T = void>
auto log_error(Logger logger, std::string event,
               std::source_location source = std::source_location::current()) {
    return [logger = std::move(logger), event = std::move(event),
            source](const auto& error) -> std::expected<T, std::decay_t<decltype(error)>> {
        try {
            logger.error(event, to_fields(error), source);
        } catch (...) {
        }
        return std::unexpected(error);
    };
}

std::filesystem::path app_log_directory();

// Construct once at each process boundary. Legacy app/recognizer calls share one value.
Logger make_process_logger(std::string component, bool system_component = false,
                           bool ensure_system_directory = false);
const Logger& process_logger(std::string_view component = "app", bool system_component = false,
                             bool ensure_system_directory = false);
} // namespace su::log
