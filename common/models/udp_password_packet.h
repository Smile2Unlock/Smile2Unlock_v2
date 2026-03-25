#pragma once

#include <cstdint>

// 魔术字: "PWDR"
#define PASSWORD_RESPONSE_MAGIC 0x52574450

// 协议版本
#define PASSWORD_RESPONSE_VERSION 1

// 受保护密码响应包
#pragma pack(push, 1)
struct UdpPasswordResponsePacket {
    uint32_t magic_number;             // 魔术字
    uint32_t version;                  // 协议版本
    uint32_t session_id;               // 会话 ID
    int32_t result_code;               // 0 成功，非 0 失败
    char username[64];                 // 用户名
    uint32_t protected_password_size;  // 受保护密码长度
    unsigned char protected_password[512]; // CryptProtectData 结果
    uint64_t timestamp;                // 时间戳
};
#pragma pack(pop)
