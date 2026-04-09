#pragma once
#include <cstdint>

// 魔术字: SHA256("FaceRecognizer") 前 4 字节
#define MAGIC_NUMBER 0x8581DAF3

// 协议版本
#define PROTOCOL_VERSION 2

// 本地回环链路上承载识别特征，预留 48 KiB 足够当前 hex 特征串传输
#define UDP_STATUS_MAX_FEATURE_BYTES (48 * 1024)

// UDP 数据包结构
#pragma pack(push, 1)
struct UdpStatusPacket {
    uint32_t magic_number;  // 魔术字，用于验证数据包
    uint32_t version;       // 协议版本
    int32_t status_code;    // 状态码
    uint32_t session_id;    // 会话 ID
    uint32_t feature_bytes; // 特征字节数
    char username[64];      // 识别到的用户名（可选）
    char feature[UDP_STATUS_MAX_FEATURE_BYTES]; // 识别特征（可选，hex 字符串）
    uint64_t timestamp;     // 时间戳
};
#pragma pack(pop)
