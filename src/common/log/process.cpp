#include "logger.h"
#include <cstdlib>
#ifdef _WIN32
#include "windows_directory.h"
#include <shlobj.h>
#endif

namespace su::log {
namespace {
struct LevelSetting {
    Level level = Level::info;
    bool invalid = false;
};
#ifdef _WIN32
std::optional<std::string> registry_level() {
    wchar_t value[64]{};
    DWORD bytes = sizeof(value);
    const auto status = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Smile2Unlock", L"LogLevel",
                                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value, &bytes);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
        return std::nullopt;
    if (status != ERROR_SUCCESS)
        return "invalid";
    auto text = std::string{};
    for (const auto c : std::wstring_view{value})
        text += c <= 127 ? static_cast<char>(c) : '?';
    return text;
}
#endif
LevelSetting configured_level(bool system) {
    auto text = std::optional<std::string>{};
#ifdef _WIN32
    if (system)
        text = registry_level();
#else
    (void)system;
#endif
    if (const auto* environment = std::getenv("SMILE2UNLOCK_LOG_LEVEL"))
        text = environment;
    if (!text)
        return {};
    const auto level = parse_level(*text);
    return {level.value_or(Level::info), !level};
}
std::filesystem::path app_directory() {
#ifdef _WIN32
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)))
        return {};
    const auto memory = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>{raw, &CoTaskMemFree};
    return std::filesystem::path{raw} / L"Smile2Unlock" / L"Logs";
#else
    if (const auto* state = std::getenv("XDG_STATE_HOME");
        state && *state && std::filesystem::path{state}.is_absolute())
        return std::filesystem::path{state} / "smile2unlock/logs";
    const auto* home = std::getenv("HOME");
    return home && *home ? std::filesystem::path{home} / ".local/state/smile2unlock/logs"
                         : std::filesystem::path{};
#endif
}
Sink app_sink() {
    const auto path = app_directory();
    if (path.empty())
        return stderr_sink();
    auto error = std::error_code{};
    std::filesystem::create_directories(path, error);
    if (error)
        return stderr_sink();
    return tee(rotating_file_sink(path / "app.log"), stderr_sink());
}
} // namespace

Logger make_process_logger(std::string component, bool system, bool ensure) {
    const auto config = configured_level(system);
    auto sink = stderr_sink();
    auto directory_error = std::optional<SinkError>{};
#ifdef _WIN32
    if (system) {
        if (ensure) {
            const auto created = ensure_system_log_directory();
            if (!created)
                directory_error = created.error();
        }
        const auto root = program_data_path();
        sink = root && !directory_error
                   ? fallback(rotating_file_sink(
                                  *root / L"Smile2Unlock/Logs" / (component + ".log"), {}, true),
                              debugger_sink())
                   : debugger_sink();
    } else
        sink = tee(app_sink(), debugger_sink());
#else
    (void)ensure;
    if (!system)
        sink = app_sink();
#endif
    auto logger = Logger{std::move(component), config.level, std::move(sink)};
    if (config.invalid)
        logger.warn("log.level_invalid");
    if (directory_error)
        logger.warn("log.directory_rejected", to_fields(*directory_error));
    return logger;
}
const Logger& process_logger(std::string_view component, bool system, bool ensure) {
    static const auto logger = make_process_logger(std::string{component}, system, ensure);
    return logger;
}
} // namespace su::log
