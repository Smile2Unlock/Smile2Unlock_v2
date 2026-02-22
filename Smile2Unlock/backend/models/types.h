#pragma once

#include <string>

namespace smile2unlock {

struct User {
    int id;
    std::string username;
    std::string face_feature;
    std::string encrypted_password;
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
};

struct RecognitionResult {
    bool success;
    std::string username;
    float confidence;
    std::string error_message;
};

} // namespace smile2unlock
