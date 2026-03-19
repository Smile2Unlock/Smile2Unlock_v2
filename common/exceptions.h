#pragma once

#include <stdexcept>
#include <string>

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