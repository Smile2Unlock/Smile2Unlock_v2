#pragma once
#include <cstdint>

// 状态码定义
enum class RecognitionStatus : int32_t {
    IDLE = 0,           // 空闲/未开始
    RECOGNIZING = 1,    // 正在识别
    SUCCESS = 2,        // 识别成功
    FAILED = 3,         // 识别失败
    TIMEOUT = 4,        // 超时
    RECOGNITION_ERROR = 5,  // 错误（避免与 Windows ERROR 宏冲突）
    FACE_DETECTED = 6,  // 检测到人脸（预热模式）
    PROCESS_ENDED = 7   // 进程已结束
};
