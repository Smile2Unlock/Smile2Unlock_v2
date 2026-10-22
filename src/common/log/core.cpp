#include "core.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace su::log {
namespace {

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
std::string lowercase(std::string_view text) {
    auto result = std::string{text};
    std::ranges::transform(result, result.begin(), lower);
    return result;
}

std::size_t utf8_width(std::string_view text, std::size_t index) noexcept {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x80) return 1;
    const auto width = byte >= 0xC2 && byte <= 0xDF ? 2u
        : byte >= 0xE0 && byte <= 0xEF ? 3u : byte >= 0xF0 && byte <= 0xF4 ? 4u : 0u;
    if (!width || index + width > text.size()) return 0;
    for (auto offset = 1u; offset < width; ++offset) {
        const auto next = static_cast<unsigned char>(text[index + offset]);
        if (next < 0x80 || next > 0xBF) return 0;
    }
    const auto second = static_cast<unsigned char>(text[index + 1]);
    if ((byte == 0xE0 && second < 0xA0) || (byte == 0xED && second >= 0xA0)
        || (byte == 0xF0 && second < 0x90) || (byte == 0xF4 && second >= 0x90)) return 0;
    return width;
}

std::string identifier(std::string_view text) {
    auto output = utf8_value(text);
    std::ranges::transform(output, output.begin(), [](unsigned char c) -> char {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ? c : '_';
    });
    return output.empty() ? "_" : output;
}

std::string timestamp_text(Timestamp timestamp, int offset) {
    using namespace std::chrono;
    // An explicit offset makes formatting deterministic, including dates before the epoch.
    offset = std::clamp(offset, -1439, 1439);
    const auto local = timestamp + minutes{offset};
    const auto day = floor<days>(local);
    const auto date = year_month_day{day};
    const auto time = hh_mm_ss{local - day};
    auto buffer = std::array<char, 64>{};
    const auto absolute = offset < 0 ? -offset : offset;
    std::snprintf(buffer.data(), buffer.size(), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lld%c%02d:%02d",
        int(date.year()), unsigned(date.month()), unsigned(date.day()),
        static_cast<long long>(time.hours().count()), static_cast<long long>(time.minutes().count()),
        static_cast<long long>(time.seconds().count()), static_cast<long long>(time.subseconds().count()),
        offset < 0 ? '-' : '+', absolute / 60, absolute % 60);
    return buffer.data();
}

} // namespace

std::optional<Level> parse_level(std::string_view text) noexcept {
    constexpr std::array names{"debug", "info", "warn", "error"};
    for (auto index = std::size_t{0}; index < names.size(); ++index) {
        if (std::ranges::equal(text, std::string_view{names[index]}, {}, lower, lower))
            return static_cast<Level>(index);
    }
    return std::nullopt;
}

std::string_view level_name(Level level) noexcept {
    switch (level) {
    case Level::debug: return "DEBUG";
    case Level::info: return "INFO";
    case Level::warn: return "WARN";
    case Level::error: return "ERROR";
    }
    return "ERROR";
}

std::string utf8_value(std::string_view value) {
    constexpr auto marker = std::string_view{"…[truncated]"};
    auto output = std::string{};
    const auto truncated = value.size() > 512;
    const auto limit = truncated ? 512 - marker.size() : 512;
    for (auto index = std::size_t{0}; index < value.size();) {
        const auto width = utf8_width(value, index);
        const auto part = width ? value.substr(index, width) : std::string_view{"�"};
        if (output.size() + part.size() > limit) {
            if (output.size() > 512 - marker.size()) {
                while (output.size() > 512 - marker.size()) {
                    auto start = output.size() - 1;
                    while ((static_cast<unsigned char>(output[start]) & 0xC0) == 0x80) --start;
                    output.resize(start);
                }
            }
            return output + std::string{marker};
        }
        output += part;
        index += width ? width : 1;
    }
    if (truncated) output += marker;
    return output;
}

std::string quote_value(std::string_view value) {
    const auto clean = utf8_value(value);
    const auto quote = clean.empty() || std::ranges::any_of(clean, [](unsigned char c) {
        return c <= 0x20 || c == 0x7F || c == '=' || c == '"' || c == '\\';
    });
    auto output = std::string{quote ? "\"" : ""};
    constexpr auto hex = std::string_view{"0123456789abcdef"};
    for (const auto c : clean) {
        switch (c) {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                output += "\\x";
                output += hex[static_cast<unsigned char>(c) >> 4];
                output += hex[static_cast<unsigned char>(c) & 15];
            } else output += c;
        }
    }
    if (quote) output += '"';
    return output;
}

std::vector<Field> redact(std::vector<Field> fields) {
    constexpr std::array denied{"password", "passwd", "secret", "token", "nonce",
        "feature", "embedding", "key", "credential"};
    for (auto& item : fields) {
        const auto name = lowercase(item.key);
        if (std::ranges::any_of(denied, [&](auto word) { return name.contains(word); }))
            item.value = "[redacted]";
    }
    return fields;
}

std::string format_record(const Record& record) {
    auto output = timestamp_text(record.timestamp, record.utc_offset_minutes) + " "
        + std::string{level_name(record.level)} + " " + identifier(record.component) + " "
        + std::to_string(record.pid) + ":" + std::to_string(record.tid)
        + " event=" + quote_value(record.event);
    for (const auto& item : redact(record.fields))
        output += " " + identifier(item.key) + "=" + quote_value(item.value);
    if (record.source && enabled(Level::warn, record.level)) {
        auto file = std::string_view{record.source->file_name()};
        const auto slash = file.find_last_of("/\\");
        if (slash != std::string_view::npos) file.remove_prefix(slash + 1);
        output += " source=" + quote_value(std::string{file} + ":" + std::to_string(record.source->line()));
    }
    return output + "\n";
}

WritePlan plan_write(std::uint64_t current, std::uint64_t size, RotationPolicy policy) noexcept {
    return current && (current >= policy.max_bytes || size > policy.max_bytes - current)
        ? WritePlan{Rotate{}} : WritePlan{Append{}};
}
std::string rotated_name(std::string_view base, unsigned index) {
    return std::string{base} + "." + std::to_string(index);
}

} // namespace su::log
