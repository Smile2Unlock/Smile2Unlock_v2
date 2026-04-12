#pragma once

#include <string>
#include <vector>

namespace smile2unlock {

struct FaceData {
    int id{};
    std::string feature;
    std::string image_path;
    std::string remark;
    std::string created_at;
};

struct User {
    int id{};
    std::string username;
    std::string encrypted_password;
    std::vector<FaceData> faces;
    std::string remark;
    std::string created_at;
};

struct DllStatus {
    bool is_injected{};
    bool is_registry_configured{};
    std::string source_path;
    std::string target_path;
    std::string source_hash;
    std::string target_hash;
    std::string source_version;
    std::string target_version;
    bool hash_match{};
    bool version_match{};
};

struct RecognitionResult {
    bool success{};
    std::string username;
    float confidence{};
    std::string error_message;
};

struct FaceRecognizerConfig {
    int camera{};
    bool liveness{true};
    float face_threshold{0.62f};
    float liveness_threshold{0.8f};
    bool debug{};
    std::string language{"zh-CN"};
    bool auto_update_check{true};
};

} // namespace smile2unlock
