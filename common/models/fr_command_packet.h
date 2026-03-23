#pragma once

#include <cstdint>

namespace smile2unlock {

/**
 * @brief FR 命令类型
 */
enum class FRCommandType : int32_t {
    PING = 0,                    // 心跳检测
    EXTRACT_FEATURE = 1,         // 提取人脸特征（从文件）
    COMPARE_FEATURES = 2,        // 比较两个特征
    START_RECOGNITION = 3,       // 开始实时识别
    STOP_RECOGNITION = 4,        // 停止实时识别
    GET_STATUS = 5,              // 获取当前状态
    SHUTDOWN = 99                // 关闭 FR 进程
};

/**
 * @brief FR 命令包 (Smile2Unlock -> FaceRecognizer)
 */
struct FRCommandPacket {
    static constexpr uint32_t MAGIC_NUMBER = 0xFACE0001;
    static constexpr int32_t PROTOCOL_VERSION = 1;

    uint32_t magic_number;       // 魔数，用于验证包完整性
    int32_t version;             // 协议版本
    int32_t command_type;        // 命令类型
    int32_t sequence_id;         // 序列号（用于匹配请求和响应）
    char payload[1024];          // 负载数据（JSON 或路径字符串）
    int64_t timestamp;           // 时间戳
};

/**
 * @brief FR 响应包 (FaceRecognizer -> Smile2Unlock)
 */
struct FRResponsePacket {
    static constexpr uint32_t MAGIC_NUMBER = 0xFACE0002;

    uint32_t magic_number;       // 魔数
    int32_t version;             // 协议版本
    int32_t sequence_id;         // 对应的请求序列号
    int32_t status_code;         // 状态码（0=成功，负数=错误）
    char result_data[2048];      // 结果数据（特征向量、相似度等）
    int64_t timestamp;           // 时间戳
};

} // namespace smile2unlock
