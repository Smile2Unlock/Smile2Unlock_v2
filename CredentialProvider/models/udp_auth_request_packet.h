#pragma once
#include <cstdint>

// 魔术字: SHA256("AuthRequest") 前 4 字节
#define AUTH_REQUEST_MAGIC 0x41555448  // "AUTH"

// 协议版本
#define AUTH_REQUEST_VERSION 1

// 认证请求类型
enum class AuthRequestType : int32_t {
    START_RECOGNITION = 1,  // 开始人脸识别
    CANCEL_RECOGNITION = 2, // 取消识别
    QUERY_STATUS = 3        // 查询状态
};

// UDP 认证请求数据包结构
#pragma pack(push, 1)
struct UdpAuthRequestPacket {
    uint32_t magic_number;        // 魔术字，用于验证数据包
    uint32_t version;             // 协议版本
    int32_t request_type;         // 请求类型 (AuthRequestType)
    char username_hint[64];       // 用户名提示（可选，用于优化识别）
    uint64_t timestamp;           // 时间戳
    uint32_t session_id;          // 会话 ID（用于关联请求和响应）
};
#pragma pack(pop)
