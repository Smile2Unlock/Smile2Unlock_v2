#pragma once

#include <string>
#include <vector>

namespace smile2unlock {

// 人脸数据
struct FaceData {
    int id;
    std::string feature;        // 特征向量
    std::string image_path;     // 图片路径
    std::string remark;         // 备注
    std::string created_at;     // 创建时间
};

// 用户
struct User {
    int id;
    std::string username;
    std::string encrypted_password;
    std::vector<FaceData> faces;  // 多张人脸
    std::string remark;           // 备注
    std::string created_at;       // 创建时间
};

struct DllStatus {
    bool is_injected;
    bool is_registry_configured;
    std::string source_path;
    std::string target_path;
    std::string source_hash;
    std::string target_hash;
    std::string source_version;
    std::string target_version;
    bool hash_match;
    bool version_match;
};

struct RecognitionResult {
    bool success;
    std::string username;
    float confidence;
    std::string error_message;
};

} // namespace smile2unlock
