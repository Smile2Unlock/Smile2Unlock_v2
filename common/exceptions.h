#pragma once

#include <stdexcept>
#include <string>
#include <variant>
#include <optional>
#include "utils/logger.h"

// ============================================================================
// 错误码定义
// ============================================================================

namespace smile2unlock {

enum class ErrorCode : int {
    SUCCESS = 0,
    
    // 通用错误 (1-99)
    UNKNOWN_ERROR = 1,
    INVALID_ARGUMENT = 2,
    OPERATION_FAILED = 3,
    TIMEOUT = 4,
    NOT_INITIALIZED = 5,
    ALREADY_INITIALIZED = 6,
    
    // 数据库错误 (100-199)
    DB_CONNECTION_FAILED = 100,
    DB_QUERY_FAILED = 101,
    DB_INSERT_FAILED = 102,
    DB_UPDATE_FAILED = 103,
    DB_DELETE_FAILED = 104,
    DB_NOT_FOUND = 105,
    
    // 人脸识别错误 (200-299)
    FR_MODEL_LOAD_FAILED = 200,
    FR_CAMERA_ERROR = 201,
    FR_FEATURE_EXTRACTION_FAILED = 202,
    FR_RECOGNITION_FAILED = 203,
    FR_NO_FACE_DETECTED = 204,
    FR_LIVENESS_CHECK_FAILED = 205,
    
    // IPC 错误 (300-399)
    IPC_CONNECTION_FAILED = 300,
    IPC_SEND_FAILED = 301,
    IPC_RECEIVE_FAILED = 302,
    IPC_TIMEOUT = 303,
    IPC_INVALID_PACKET = 304,
    
    // DLL 注入错误 (400-499)
    DLL_INJECTION_FAILED = 400,
    DLL_HASH_MISMATCH = 401,
    DLL_REGISTRY_FAILED = 402,
};

/**
 * @brief 错误信息结构
 */
struct Error {
    ErrorCode code = ErrorCode::SUCCESS;
    std::string message;
    std::string context;  // 附加上下文信息
    
    Error() = default;
    Error(ErrorCode c, std::string msg, std::string ctx = "")
        : code(c), message(std::move(msg)), context(std::move(ctx)) {}
    
    bool ok() const { return code == ErrorCode::SUCCESS; }
    operator bool() const { return ok(); }
    
    std::string toString() const {
        if (context.empty()) {
            return message;
        }
        return message + " (" + context + ")";
    }
};

/**
 * @brief 结果类型 (类似 Rust 的 Result<T, E>)
 */
template<typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}  // NOLINT: 允许隐式转换
    Result(Error error) : data_(std::move(error)) {}  // NOLINT: 允许隐式转换
    
    bool ok() const { return std::holds_alternative<T>(data_); }
    operator bool() const { return ok(); }
    
    T& value() & { return std::get<T>(data_); }
    T&& value() && { return std::get<T>(std::move(data_)); }
    const T& value() const& { return std::get<T>(data_); }
    
    Error& error() & { return std::get<Error>(data_); }
    Error&& error() && { return std::get<Error>(std::move(data_)); }
    const Error& error() const& { return std::get<Error>(data_); }
    
    T value_or(T default_value) const {
        return ok() ? value() : std::move(default_value);
    }
    
private:
    std::variant<T, Error> data_;
};

/**
 * @brief 无值结果类型
 */
template<>
class Result<void> {
public:
    Result() : error_(std::nullopt) {}
    Result(Error error) : error_(std::move(error)) {}  // NOLINT
    
    bool ok() const { return !error_.has_value(); }
    operator bool() const { return ok(); }
    
    const Error& error() const& { return error_.value(); }
    Error&& error() && { return std::move(error_.value()); }
    
private:
    std::optional<Error> error_;
};

// 辅助函数：创建成功结果
inline Result<void> Success() { return Result<void>{}; }

// 辅助函数：创建错误结果
template<typename T = void>
Result<T> MakeError(ErrorCode code, std::string message, std::string context = "") {
    return Result<T>(Error(code, std::move(message), std::move(context)));
}

} // namespace smile2unlock

// ============================================================================
// 异常类定义 (保留向后兼容)
// ============================================================================

namespace FaceRecognition {

/**
 * @brief 人脸识别基础异常类
 */
class FaceRecognitionException : public std::runtime_error {
public:
    explicit FaceRecognitionException(const std::string& message)
        : std::runtime_error(message) {}
    
    explicit FaceRecognitionException(const char* message)
        : std::runtime_error(message) {}
};

/**
 * @brief 模型加载异常
 */
class ModelLoadException : public FaceRecognitionException {
public:
    explicit ModelLoadException(const std::string& modelName, const std::string& reason)
        : FaceRecognitionException("Failed to load model '" + modelName + "': " + reason) {}
};

/**
 * @brief 摄像头异常
 */
class CameraException : public FaceRecognitionException {
public:
    explicit CameraException(const std::string& reason)
        : FaceRecognitionException("Camera error: " + reason) {}
};

/**
 * @brief 特征提取异常
 */
class FeatureExtractionException : public FaceRecognitionException {
public:
    explicit FeatureExtractionException(const std::string& reason)
        : FaceRecognitionException("Feature extraction failed: " + reason) {}
};

/**
 * @brief 文件操作异常
 */
class FileOperationException : public FaceRecognitionException {
public:
    explicit FileOperationException(const std::string& filePath, const std::string& reason)
        : FaceRecognitionException("File operation failed for '" + filePath + "': " + reason) {}
};

/**
 * @brief 配置异常
 */
class ConfigException : public FaceRecognitionException {
public:
    explicit ConfigException(const std::string& reason)
        : FaceRecognitionException("Configuration error: " + reason) {}
};

/**
 * @brief 网络通信异常
 */
class NetworkException : public FaceRecognitionException {
public:
    explicit NetworkException(const std::string& reason)
        : FaceRecognitionException("Network communication error: " + reason) {}
};

/**
 * @brief 注册表操作异常
 */
class RegistryException : public FaceRecognitionException {
public:
    explicit RegistryException(const std::string& operation, const std::string& keyPath, const std::string& reason)
        : FaceRecognitionException("Registry operation failed for '" + operation + "' on key '" + keyPath + "': " + reason) {}

    explicit RegistryException(const std::string& reason)
        : FaceRecognitionException("Registry error: " + reason) {}
};

/**
 * @brief 路径操作异常
 */
class PathOperationException : public FaceRecognitionException {
public:
    explicit PathOperationException(const std::string& path, const std::string& reason)
        : FaceRecognitionException("Path operation failed for '" + path + "': " + reason) {}

    explicit PathOperationException(const std::string& reason)
        : FaceRecognitionException("Path operation error: " + reason) {}
};

} // namespace FaceRecognition