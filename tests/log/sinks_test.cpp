#include "common/log/logger.h"
#undef NDEBUG
#include <cassert>
#include <fstream>
#include <iterator>
#include <limits>
#include <thread>
#ifdef _WIN32
#include "common/log/windows_directory.h"
#endif

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
#ifdef _WIN32
std::string read_text(const std::filesystem::path& path) {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}
int child_writer() {
    wchar_t path[32768]{};
    if (!GetEnvironmentVariableW(L"S2U_LOG_WRITER_PATH", path, 32768))
        return 2;
    return rotating_file_sink(path)(Level::info, "child\n") ? 0 : 3;
}
void shared_process(const std::filesystem::path& path) {
    auto parent = rotating_file_sink(path);
    assert(parent(Level::info, "parent\n"));
    wchar_t executable[32768]{};
    assert(GetModuleFileNameW(nullptr, executable, 32768));
    assert(SetEnvironmentVariableW(L"S2U_LOG_WRITER_PATH", path.c_str()));
    auto command = L"\"" + std::wstring{executable} + L"\" --writer";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    assert(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                          &startup, &process));
    const auto process_owner = Handle{process.hProcess};
    const auto thread_owner = Handle{process.hThread};
    assert(SetEnvironmentVariableW(L"S2U_LOG_WRITER_PATH", nullptr));
    assert(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD code = 1;
    assert(GetExitCodeProcess(process.hProcess, &code) && code == 0);
    assert(parent(Level::info, "after\n"));
    assert(read_text(path) == "parent\nchild\nafter\n");
}
void shared_rotation(const std::filesystem::path& path) {
    auto first = rotating_file_sink(path, {20, 3});
    auto second = rotating_file_sink(path, {20, 3});
    assert(first(Level::info, "first\n"));
    assert(second(Level::info, "second\n"));
    assert(first(Level::info, "third-long-line\n"));
    assert(second(Level::info, "rejoined\n"));
    assert(read_text(path) == "third-long-line\nrejoined\n");
    auto archive = path;
    archive += L".1";
    assert(read_text(archive) == "first\nsecond\n");
}
void blocked_rotation(const std::filesystem::path& path) {
    auto sink = rotating_file_sink(path, {10, 2});
    assert(sink(Level::info, "123456\n"));
    auto archive = path;
    archive += L".1";
    std::ofstream{archive} << "archive\n";
    auto blocker = Handle{CreateFileW(archive.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr)};
    assert(blocker.get() != INVALID_HANDLE_VALUE);
    assert(sink(Level::info, "abcdef\n"));
    assert(read_text(path) == "123456\nabcdef\n");
    blocker.reset();
    assert(sink(Level::info, "ghijkl\n"));
    assert(read_text(path) == "ghijkl\n");
    assert(read_text(archive) == "123456\nabcdef\n");
}
void windows_writers() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("su-log-writers-" + std::to_string(GetCurrentProcessId()) + "-" +
                       std::to_string(GetTickCount64()));
    std::filesystem::create_directory(root);
    shared_process(root / "shared.log");
    shared_rotation(root / "rotation.log");
    blocked_rotation(root / "blocked.log");
    std::filesystem::remove_all(root);
}
#endif
} // namespace
int main(int argc, char** argv) {
#ifdef _WIN32
    if (argc == 2 && std::string_view{argv[1]} == "--writer")
        return child_writer();
    windows_writers();
#else
    (void)argc;
    (void)argv;
#endif
    composition();
    file_contracts();
}
