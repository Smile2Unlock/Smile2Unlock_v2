#pragma once

#include <cstdint>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <string>

namespace su::app {
inline std::string capture_local_time(std::uint64_t unix_seconds) {
    if (unix_seconds > static_cast<std::uint64_t>(std::numeric_limits<std::time_t>::max()))
        throw std::out_of_range("invalid capture time");
    const auto time = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time) != 0)
#else
    if (localtime_r(&time, &local) == nullptr)
#endif
        throw std::out_of_range("invalid capture time");
    char text[32]{};
    if (std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local) == 0)
        throw std::out_of_range("invalid capture time");
    return text;
}
}
