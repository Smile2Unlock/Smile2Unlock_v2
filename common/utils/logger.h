#pragma once

/**
 * @file logger.h
 * @brief 统一日志系统
 *
 * 使用方法:
 *   LOG_INFO("模块名", "消息 %d", value);
 *   LOG_ERROR("模块名", "错误: %s", error_msg);
 */

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <string>
#include <mutex>
#include <memory>
#include <filesystem>
#include <fstream>
#include <streambuf>
#include <ostream>
#include <iostream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace smile2unlock {

enum class LogLevel : int {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERR = 3,       // 避免与 Windows ERROR 宏冲突
    FATAL = 4,
};

namespace detail {

inline std::mutex& GetLogMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::string& GetProcessLogSource() {
    static std::string source = "APP";
    return source;
}

inline std::string& GetProcessLogDirectory() {
    static std::string directory;
    return directory;
}

inline bool& IsProcessFileLoggingEnabled() {
    static bool enabled = false;
    return enabled;
}

inline const char* LogLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARN";
        case LogLevel::ERR:     return "ERROR";
        case LogLevel::FATAL:   return "FATAL";
        default:                return "UNKNOWN";
    }
}

inline std::string CurrentDateStamp() {
    time_t now = time(nullptr);
    struct tm tm_info;
#ifdef _WIN32
    localtime_s(&tm_info, &now);
#else
    localtime_r(&now, &tm_info);
#endif

    char date_buf[16];
    strftime(date_buf, sizeof(date_buf), "%Y%m%d", &tm_info);
    return date_buf;
}

inline std::string CurrentTimestampWithMilliseconds() {
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    char time_buf[40];
    snprintf(time_buf, sizeof(time_buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return time_buf;
#else
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return time_buf;
#endif
}

inline void EnsureLogDirectoryExists() {
    if (!IsProcessFileLoggingEnabled() || GetProcessLogDirectory().empty()) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(GetProcessLogDirectory(), ec);
}

inline void CleanupExpiredLogFiles() {
    if (!IsProcessFileLoggingEnabled() || GetProcessLogDirectory().empty()) {
        return;
    }

    std::error_code ec;
    const std::filesystem::path log_directory = GetProcessLogDirectory();
    if (!std::filesystem::exists(log_directory, ec)) {
        return;
    }

    const auto now = std::filesystem::file_time_type::clock::now();
    const auto retention = std::chrono::hours(24 * 3);

    for (const auto& entry : std::filesystem::directory_iterator(log_directory, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (entry.path().extension() != ".log") {
            continue;
        }

        const auto last_write_time = entry.last_write_time(ec);
        if (ec) {
            ec.clear();
            continue;
        }

        if (now > last_write_time && now - last_write_time > retention) {
            std::filesystem::remove(entry.path(), ec);
            ec.clear();
        }
    }
}

inline std::filesystem::path CurrentLogFilePath() {
    return std::filesystem::path(GetProcessLogDirectory()) /
           (GetProcessLogSource() + "-" + CurrentDateStamp() + ".log");
}

inline void AppendFileLogLine(const char* channel, const std::string& message) {
    if (!IsProcessFileLoggingEnabled() || GetProcessLogDirectory().empty()) {
        return;
    }

    EnsureLogDirectoryExists();
    std::ofstream file(CurrentLogFilePath(), std::ios::app | std::ios::binary);
    if (!file.is_open()) {
        return;
    }

    file << "[" << CurrentTimestampWithMilliseconds() << "] "
         << "[" << GetProcessLogSource() << "] ";
    if (channel != nullptr && channel[0] != '\0') {
        file << "[" << channel << "] ";
    }
    file << message << '\n';
    file.flush();
}

class RedirectStreamBuf final : public std::streambuf {
public:
    RedirectStreamBuf(std::streambuf* original, std::string channel)
        : original_(original), channel_(std::move(channel)) {}

    ~RedirectStreamBuf() override {
        sync();
    }

protected:
    int overflow(int ch) override {
        if (ch == traits_type::eof()) {
            return sync() == 0 ? traits_type::not_eof(ch) : traits_type::eof();
        }

        original_->sputc(static_cast<char>(ch));
        line_buffer_.push_back(static_cast<char>(ch));
        if (ch == '\n') {
            flush_line_buffer();
        }
        return ch;
    }

    int sync() override {
        if (original_ != nullptr) {
            original_->pubsync();
        }
        flush_line_buffer();
        return 0;
    }

private:
    void flush_line_buffer() {
        while (!line_buffer_.empty() &&
               (line_buffer_.back() == '\n' || line_buffer_.back() == '\r')) {
            line_buffer_.pop_back();
        }

        if (line_buffer_.empty()) {
            return;
        }

        std::lock_guard<std::mutex> lock(GetLogMutex());
        AppendFileLogLine(channel_.c_str(), line_buffer_);
        line_buffer_.clear();
    }

    std::streambuf* original_;
    std::string channel_;
    std::string line_buffer_;
};

inline std::unique_ptr<RedirectStreamBuf>& StdoutRedirect() {
    static std::unique_ptr<RedirectStreamBuf> redirect;
    return redirect;
}

inline std::unique_ptr<RedirectStreamBuf>& StderrRedirect() {
    static std::unique_ptr<RedirectStreamBuf> redirect;
    return redirect;
}

inline std::streambuf*& OriginalStdoutBuf() {
    static std::streambuf* buffer = nullptr;
    return buffer;
}

inline std::streambuf*& OriginalStderrBuf() {
    static std::streambuf* buffer = nullptr;
    return buffer;
}

inline void LogImpl(LogLevel level, const char* module, const char* format, ...) {
    std::lock_guard<std::mutex> lock(GetLogMutex());
    
    // 获取时间戳
    time_t now = time(nullptr);
    struct tm tm_info;
#ifdef _WIN32
    localtime_s(&tm_info, &now);
#else
    localtime_r(&now, &tm_info);
#endif
    
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    
    // 打印前缀
    fprintf(stderr, "[%s] [%s] [%s] ", time_buf, LogLevelToString(level), module);
    
    // 打印消息
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    
    fprintf(stderr, "\n");
    fflush(stderr);
}

#ifdef _WIN32
inline void LogDebugViewImpl(LogLevel level, const char* module, const char* format, ...) {
    char buffer[1024];
    int offset = snprintf(buffer, sizeof(buffer), "[%s] [%s] ", 
                          LogLevelToString(level), module);
    
    va_list args;
    va_start(args, format);
    vsnprintf(buffer + offset, sizeof(buffer) - offset, format, args);
    va_end(args);
    
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}
#endif

} // namespace detail

inline void ConfigureProcessFileLogging(const char* source, const std::string& log_directory) {
    std::lock_guard<std::mutex> lock(detail::GetLogMutex());
    detail::GetProcessLogSource() = (source != nullptr && source[0] != '\0') ? source : "APP";
    detail::GetProcessLogDirectory() = log_directory;
    detail::IsProcessFileLoggingEnabled() = !log_directory.empty();
    detail::EnsureLogDirectoryExists();
    detail::CleanupExpiredLogFiles();
}

inline std::string ResolveLogDirectoryFromModule(HMODULE module_handle = nullptr) {
#ifdef _WIN32
    char module_path[MAX_PATH] = {};
    if (GetModuleFileNameA(module_handle, module_path, MAX_PATH) == 0) {
        return {};
    }
    return (std::filesystem::path(module_path).parent_path() / "logs").string();
#else
    (void)module_handle;
    return "logs";
#endif
}

inline void InstallStandardStreamFileLogging() {
    std::lock_guard<std::mutex> lock(detail::GetLogMutex());
    if (!detail::IsProcessFileLoggingEnabled()) {
        return;
    }

    if (detail::OriginalStdoutBuf() == nullptr) {
        detail::OriginalStdoutBuf() = std::cout.rdbuf();
        detail::StdoutRedirect() = std::make_unique<detail::RedirectStreamBuf>(
            detail::OriginalStdoutBuf(), "STDOUT");
        std::cout.rdbuf(detail::StdoutRedirect().get());
    }

    if (detail::OriginalStderrBuf() == nullptr) {
        detail::OriginalStderrBuf() = std::cerr.rdbuf();
        detail::StderrRedirect() = std::make_unique<detail::RedirectStreamBuf>(
            detail::OriginalStderrBuf(), "STDERR");
        std::cerr.rdbuf(detail::StderrRedirect().get());
    }
}

inline void WriteFileLogLine(const char* channel, const std::string& message) {
    std::lock_guard<std::mutex> lock(detail::GetLogMutex());
    detail::AppendFileLogLine(channel, message);
}

// ============================================================================
// 日志宏
// ============================================================================

#ifdef _WIN32
// Windows: 同时输出到 stderr 和调试器
#define LOG_DEBUG(module, fmt, ...) \
    do { \
        smile2unlock::detail::LogImpl(smile2unlock::LogLevel::DEBUG, module, fmt, ##__VA_ARGS__); \
        smile2unlock::detail::LogDebugViewImpl(smile2unlock::LogLevel::DEBUG, module, fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_INFO(module, fmt, ...) \
    do { \
        smile2unlock::detail::LogImpl(smile2unlock::LogLevel::INFO, module, fmt, ##__VA_ARGS__); \
        smile2unlock::detail::LogDebugViewImpl(smile2unlock::LogLevel::INFO, module, fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_WARNING(module, fmt, ...) \
    do { \
        smile2unlock::detail::LogImpl(smile2unlock::LogLevel::WARNING, module, fmt, ##__VA_ARGS__); \
        smile2unlock::detail::LogDebugViewImpl(smile2unlock::LogLevel::WARNING, module, fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_ERROR(module, fmt, ...) \
    do { \
        smile2unlock::detail::LogImpl(smile2unlock::LogLevel::ERR, module, fmt, ##__VA_ARGS__); \
        smile2unlock::detail::LogDebugViewImpl(smile2unlock::LogLevel::ERR, module, fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_FATAL(module, fmt, ...) \
    do { \
        smile2unlock::detail::LogImpl(smile2unlock::LogLevel::FATAL, module, fmt, ##__VA_ARGS__); \
        smile2unlock::detail::LogDebugViewImpl(smile2unlock::LogLevel::FATAL, module, fmt, ##__VA_ARGS__); \
    } while(0)

#else
// 非Windows: 仅输出到 stderr
#define LOG_DEBUG(module, fmt, ...) \
    smile2unlock::detail::LogImpl(smile2unlock::LogLevel::DEBUG, module, fmt, ##__VA_ARGS__)

#define LOG_INFO(module, fmt, ...) \
    smile2unlock::detail::LogImpl(smile2unlock::LogLevel::INFO, module, fmt, ##__VA_ARGS__)

#define LOG_WARNING(module, fmt, ...) \
    smile2unlock::detail::LogImpl(smile2unlock::LogLevel::WARNING, module, fmt, ##__VA_ARGS__)

#define LOG_ERROR(module, fmt, ...) \
    smile2unlock::detail::LogImpl(smile2unlock::LogLevel::ERR, module, fmt, ##__VA_ARGS__)

#define LOG_FATAL(module, fmt, ...) \
    smile2unlock::detail::LogImpl(smile2unlock::LogLevel::FATAL, module, fmt, ##__VA_ARGS__)

#endif

// ============================================================================
// 模块名称常量
// ============================================================================

#define LOG_MODULE_UDP_SENDER      "UDP-Sender"
#define LOG_MODULE_UDP_RECEIVER    "UDP-Receiver"
#define LOG_MODULE_IPC_HANDLER     "IPC-Handler"
#define LOG_MODULE_FACE_RECOGNIZER "FR"
#define LOG_MODULE_SERVICE         "Service"
#define LOG_MODULE_DATABASE        "DB"
#define LOG_MODULE_CREDENTIAL      "CP"

} // namespace smile2unlock
