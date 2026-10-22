#include "common/log/core.h"
#undef NDEBUG
#include <cassert>
#include <fstream>
#include <iterator>
#include <limits>
#include <thread>

using namespace su::log;
namespace {
std::string decode(std::string_view text) {
    auto result = std::string{};
    for (auto index = std::size_t{0}; index < text.size(); ++index) {
        if (text[index] != '\\' || index + 1 >= text.size()) { result += text[index]; continue; }
        switch (text[++index]) {
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'x': result += static_cast<char>(std::stoi(std::string{text.substr(index + 1, 2)}, nullptr, 16)); index += 2; break;
        default: result += text[index];
        }
    }
    return result;
}
void golden() {
    auto input = std::ifstream{SU_LOG_GOLDEN};
    assert(input);
    auto count = 0;
    for (auto line = std::string{}; std::getline(input, line);) {
        if (line.empty() || line[0] == '#') continue;
        auto parts = std::vector<std::string>{};
        auto start = std::size_t{0};
        for (auto index = std::size_t{0}; index <= line.size(); ++index) {
            if (index == line.size() || line[index] == '|') {
                parts.push_back(line.substr(start, index - start)); start = index + 1;
            }
        }
        assert(parts.size() == 10);
        const auto actual = format_record({Timestamp{std::chrono::milliseconds{std::stoll(parts[0])}},
            std::stoi(parts[1]), *parse_level(parts[2]), parts[3], parts[6],
            std::stoull(parts[4]), std::stoull(parts[5]), {field(parts[7], decode(parts[8]))}});
        if (actual != decode(parts[9])) { std::fprintf(stderr, "golden mismatch: %s", actual.c_str()); std::abort(); }
        ++count;
    }
    assert(count >= 7);
}
void pure_contracts() {
    assert(parse_level("DeBuG") == Level::debug && !parse_level("verbose"));
    assert(enabled(Level::warn, Level::error) && !enabled(Level::info, Level::debug));
    assert(level_name(Level::warn) == "WARN");
    assert(utf8_value(std::string(600, 'a')).size() == 512);
    assert(utf8_value(std::string(512, '\xff')).size() <= 512);
    assert(utf8_value("\xed\xa0\x80") == "���");
    for (const auto key : {"PASSWORD", "passwd", "secret", "token", "nonce", "feature", "embedding", "key", "credential"})
        assert(redact({field(key, "sensitive")})[0].value == "[redacted]");
    assert(redact({field("sid", "S-1")})[0].value == "S-1");
    assert(std::holds_alternative<Append>(plan_write(0, 100, {10, 3})));
    assert(std::holds_alternative<Append>(plan_write(5, 5, {10, 3})));
    assert(std::holds_alternative<Rotate>(plan_write(5, 6, {10, 3})));
    assert(std::holds_alternative<Rotate>(plan_write(1, std::numeric_limits<std::uint64_t>::max(), {10, 3})));
    assert(rotated_name("app.log", 2) == "app.log.2");
    const auto source = std::source_location::current();
    assert(format_record({Timestamp{}, 0, Level::warn, "app", "warn", 0, 0, {}, source}).contains("source=core_test.cpp:"));
    assert(!format_record({Timestamp{}, 0, Level::info, "app", "info", 0, 0, {}, source}).contains("source="));
}
} // namespace
int main() { golden(); pure_contracts(); }
