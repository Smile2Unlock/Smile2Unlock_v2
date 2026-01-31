#pragma once
#include <cstdint>

// 魔术字: SHA256("FaceRecognizer") 前 4 字节
#define MAGIC_NUMBER 0x8581DAF3

// 协议版本
#define PROTOCOL_VERSION 1

// UDP 数据包结构
#pragma pack(push, 1)
struct UdpStatusPacket {
    uint32_t magic_number;  // 魔术字，用于验证数据包
    uint32_t version;       // 协议版本
    int32_t status_code;    // 状态码
    char username[64];      // 识别到的用户名（可选）
    uint64_t timestamp;     // 时间戳
};
#pragma pack(pop)
