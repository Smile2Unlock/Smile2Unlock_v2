#include "common/log/logger.h"
#undef NDEBUG
#include <cassert>
#include <fstream>
#include <iterator>
#include <limits>
#include <thread>

using namespace su::log;
namespace {
void composition() {
    auto calls = 0;
    const Sink failed = [](Level, std::string_view) -> std::expected<void, SinkError> {
        return std::unexpected(SinkError::open_failed);
    };
    const Sink good = [&](Level, std::string_view) -> std::expected<void, SinkError> {
        ++calls;
        return {};
    };
    assert(fallback(good, good)(Level::info, "") && calls == 1);
    assert(fallback(failed, good)(Level::info, "") && calls == 2);
    assert(!tee(failed, good)(Level::info, "") && calls == 3);
    auto output = std::string{};
    const auto logger = Logger{"test",
                               Level::info,
                               [&](Level, std::string_view line) -> std::expected<void, SinkError> {
                                   output += line;
                                   return {};
                               },
                               {},
                               [] { return Timestamp{}; }};
    logger.with({field("request_id", 42)}).info("bound");
    assert(output.contains("request_id=42"));
    output.clear();
    logger.debug("hidden");
    assert(output.empty());
    logger.info("original");
    assert(!output.contains("request_id="));
    const auto result =
        std::expected<int, SinkError>{std::unexpected(SinkError::open_failed)}.or_else(
            log_error<int>(logger, "failed"));
    assert(!result && result.error() == SinkError::open_failed && output.contains("event=failed"));
    const auto throwing =
        Logger{"test", Level::debug,
               [](Level, std::string_view) -> std::expected<void, SinkError> { throw 42; }};
    throwing.error("ignored");
}
void file_contracts() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("su-log-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    auto sink = rotating_file_sink(root / "app.log", {10, 2});
    assert(sink(Level::info, "123456\n"));
    assert(sink(Level::info, "abcdef\n"));
    assert(sink(Level::info, "ghijkl\n"));
    assert(sink(Level::info, "mnopqr\n"));
    assert(std::filesystem::file_size(root / "app.log") == 7);
    assert(std::filesystem::exists(root / "app.log.2") &&
           !std::filesystem::exists(root / "app.log.3"));
    assert(!rotating_file_sink(root / "missing/file")(Level::info, "line\n"));
#ifndef _WIN32
    std::filesystem::create_symlink(root / "app.log", root / "link");
    assert(!rotating_file_sink(root / "link")(Level::info, "attack\n"));
#endif
    sink = {};
    std::filesystem::remove_all(root);
}
} // namespace
int main() {
    composition();
    file_contracts();
}
