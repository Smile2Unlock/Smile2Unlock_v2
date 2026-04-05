#pragma once

#include <cstdint>
#include <cstring>

namespace smile2unlock {

// IPC 协议魔数和版本
constexpr int32_t GUI_IPC_MAGIC = 0x53325549;  // "S2UI"
constexpr int32_t GUI_IPC_VERSION = 1;

// 命令类型
enum class GuiIpcCommand : int32_t {
    PING = 0,
    
    // 用户管理
    GET_ALL_USERS = 100,
    ADD_USER = 101,
    UPDATE_USER = 102,
    DELETE_USER = 103,
    
    // 人脸管理
    GET_USER_FACES = 200,
    DELETE_FACE = 201,
    CAPTURE_AND_ADD_FACE = 202,
    
    // 摄像头/预览
    START_CAMERA_PREVIEW = 300,
    STOP_CAMERA_PREVIEW = 3001,
    GET_LATEST_PREVIEW = 302,
    CAPTURE_PREVIEW_FRAME = 303,
    
    // DLL 注入
    GET_DLL_STATUS = 400,
    INJECT_DLL = 401,
    
    // 人脸识别
    START_RECOGNITION = 500,
    STOP_RECOGNITION = 501,
    IS_RECOGNITION_RUNNING = 502,
    GET_RECOGNITION_RESULT = 503,
    
    // 相机状态
    IS_CAMERA_PREVIEW_RUNNING = 600,
};

// 响应状态码
enum class GuiIpcStatus : int32_t {
    SUCCESS = 0,
    FAILURE = 1,
    NOT_IMPLEMENTED = 2,
    INVALID_PAYLOAD = 3,
    SERVICE_ERROR = 4,
};

// IPC 数据包 - 请求
struct GuiIpcRequest {
    int32_t magic = GUI_IPC_MAGIC;
    int32_t version = GUI_IPC_VERSION;
    int32_t command = 0;
    int32_t payload_size = 0;
    int64_t timestamp = 0;
    int32_t request_id = 0;
    int32_t reserved = 0;
    char payload[8192] = {0};
    
    void clear() {
        magic = GUI_IPC_MAGIC;
        version = GUI_IPC_VERSION;
        command = 0;
        payload_size = 0;
        timestamp = 0;
        request_id = 0;
        reserved = 0;
        std::memset(payload, 0, sizeof(payload));
    }
};

// IPC 数据包 - 响应
struct GuiIpcResponse {
    int32_t magic = GUI_IPC_MAGIC;
    int32_t version = GUI_IPC_VERSION;
    int32_t status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
    int32_t payload_size = 0;
    int64_t timestamp = 0;
    int32_t request_id = 0;
    int32_t reserved = 0;
    char payload[8192] = {0};
    
    void clear() {
        magic = GUI_IPC_MAGIC;
        version = GUI_IPC_VERSION;
        status = static_cast<int32_t>(GuiIpcStatus::SUCCESS);
        payload_size = 0;
        timestamp = 0;
        request_id = 0;
        reserved = 0;
        std::memset(payload, 0, sizeof(payload));
    }
    
    void set_error(const char* msg) {
        status = static_cast<int32_t>(GuiIpcStatus::FAILURE);
        if (msg) {
            std::strncpy(payload, msg, sizeof(payload) - 1);
            payload_size = static_cast<int32_t>(std::strlen(payload));
        }
    }
};

// 命名管道和互斥体名称
constexpr const char* GUI_IPC_PIPE_NAME = "\\\\.\\pipe\\Smile2UnlockIPC";
constexpr const char* SERVICE_MUTEX_NAME = "Global\\Smile2UnlockService";

} // namespace smile2unlock
