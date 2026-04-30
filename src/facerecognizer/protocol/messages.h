#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace su::facerecognizer::protocol {

/**
 * @brief 命令类型枚举
 * 
 * 定义主应用程序发送给 FaceRecognizer 服务的命令类型。
 */
enum class CommandType : std::uint16_t {
    kUnknown = 0,            ///< 未知命令
    kStartCameraPreview = 1, ///< 开始摄像头预览
    kStopCameraPreview = 2,  ///< 停止摄像头预览
    kCaptureFrame = 3,       ///< 捕获单帧图像
    kStartRecognition = 4,   ///< 开始人脸识别
    kStopRecognition = 5,    ///< 停止人脸识别
    kCompareFeatures = 6,    ///< 比较特征向量
};

/**
 * @brief 消息类型枚举
 * 
 * 定义 FaceRecognizer 服务返回给主应用程序的消息类型。
 */
enum class MessageType : std::uint16_t {
    kError = 0,             ///< 错误消息
    kPreviewFrame = 1,      ///< 预览帧数据
    kCaptureResult = 2,     ///< 捕获结果
    kRecognitionResult = 3, ///< 识别结果
    kCompareResult = 4,     ///< 比较结果
};

/**
 * @brief 人脸位置框
 */
struct BoundingBox {
    int x = 0;      ///< 左上角 X 坐标
    int y = 0;      ///< 左上角 Y 坐标
    int width = 0;  ///< 宽度
    int height = 0; ///< 高度
};

/**
 * @brief 命令消息
 * 
 * 从主应用程序发送的控制命令。
 */
struct CommandMessage {
    CommandType command = CommandType::kUnknown;  ///< 命令类型
    int camera_index = 0;                         ///< 摄像头索引
    int preview_fps = 15;                         ///< 预览帧率
    bool extract_feature = false;                 ///< 是否提取特征
    bool liveness_detection = false;              ///< 是否启用活体检测
    float face_threshold = 0.62f;                 ///< 人脸匹配阈值
    float liveness_threshold = 0.80f;             ///< 活体检测阈值
    std::string username_hint;                    ///< 用户名提示（用于 1:1 验证）
    std::vector<float> feature_a;                 ///< 特征向量 A（用于比较）
    std::vector<float> feature_b;                 ///< 特征向量 B（用于比较）
};

/**
 * @brief 预览帧消息
 */
struct PreviewFrameMessage {
    int width = 0;                        ///< 图像宽度
    int height = 0;                       ///< 图像高度
    int channels = 0;                     ///< 通道数
    std::vector<std::byte> jpeg_bytes;    ///< JPEG 编码的图像数据
};

/**
 * @brief 捕获结果消息
 */
struct CaptureResultMessage {
    bool ok = false;                              ///< 是否成功
    std::string error_message;                    ///< 错误信息
    int width = 0;                                ///< 图像宽度
    int height = 0;                               ///< 图像高度
    int channels = 0;                             ///< 通道数
    std::vector<std::byte> image_bytes;           ///< 图像数据
    std::vector<float> feature;                   ///< 提取的特征向量
};

/**
 * @brief 识别结果消息
 */
struct RecognitionResultMessage {
    bool matched = false;                         ///< 是否匹配成功
    std::string username;                         ///< 匹配的用户名
    float similarity = 0.0f;                      ///< 相似度得分
    std::optional<BoundingBox> face_box;          ///< 人脸位置框
    std::vector<float> feature;                   ///< 提取的特征向量
};

/**
 * @brief 比较结果消息
 */
struct CompareResultMessage {
    bool ok = false;              ///< 是否成功
    float similarity = 0.0f;      ///< 相似度得分
    std::string error_message;    ///< 错误信息
};

/**
 * @brief 出站消息（FaceRecognizer -> 主应用程序）
 */
struct OutboundMessage {
    MessageType type = MessageType::kError;                             ///< 消息类型
    std::string error_message;                                          ///< 错误信息
    std::optional<PreviewFrameMessage> preview_frame;                   ///< 预览帧数据
    std::optional<CaptureResultMessage> capture_result;                 ///< 捕获结果
    std::optional<RecognitionResultMessage> recognition_result;         ///< 识别结果
    std::optional<CompareResultMessage> compare_result;                 ///< 比较结果
};

}  // namespace su::facerecognizer::protocol
