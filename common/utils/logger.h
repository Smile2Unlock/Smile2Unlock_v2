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
