#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace su::log {

enum class Level { debug, info, warn, error };
std::optional<Level> parse_level(std::string_view text) noexcept;
std::string_view level_name(Level level) noexcept;
constexpr bool enabled(Level minimum, Level level) noexcept { return level >= minimum; }

struct Field { std::string key; std::string value; };
inline Field field(std::string_view key, std::string_view value) {
    return {std::string{key}, std::string{value}};
}
inline Field field(std::string_view key, const char* value) {
    return field(key, std::string_view{value ? value : ""});
}
template <typename T> requires std::integral<T> || std::floating_point<T>
Field field(std::string_view key, T value) { return field(key, std::to_string(value)); }

using Timestamp = std::chrono::sys_time<std::chrono::milliseconds>;
struct Record {
    const Timestamp timestamp;
    const int utc_offset_minutes;
    const Level level;
    const std::string component;
    const std::string event;
    const std::uint64_t pid;
    const std::uint64_t tid;
    const std::vector<Field> fields = {};
    const std::optional<std::source_location> source = std::nullopt;
};

// These transformations never read clocks, environment, process state, or files.
std::string utf8_value(std::string_view value);
std::string quote_value(std::string_view value);
std::vector<Field> redact(std::vector<Field> fields);
std::string format_record(const Record& record);

struct RotationPolicy { std::uint64_t max_bytes = 1024 * 1024; unsigned keep = 3; };
struct Append {};
struct Rotate {};
using WritePlan = std::variant<Append, Rotate>;
WritePlan plan_write(std::uint64_t current_size, std::uint64_t line_size,
                     RotationPolicy policy) noexcept;
std::string rotated_name(std::string_view base, unsigned index);

} // namespace su::log
