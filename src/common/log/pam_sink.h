#pragma once
#include "sinks.h"
#include <security/pam_appl.h>
#include <security/pam_ext.h>
#include <syslog.h>
namespace su::log {
inline Sink pam_sink(pam_handle_t* handle) {
    return [handle](Level level, std::string_view line) -> std::expected<void, SinkError> {
        const auto priority = level == Level::debug  ? LOG_DEBUG
                              : level == Level::info ? LOG_INFO
                              : level == Level::warn ? LOG_WARNING
                                                     : LOG_ERR;
        pam_syslog(handle, LOG_AUTHPRIV | priority, "%.*s", static_cast<int>(line.size()),
                   line.data());
        return {};
    };
}
} // namespace su::log
