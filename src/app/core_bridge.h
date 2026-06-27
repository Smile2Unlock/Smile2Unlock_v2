#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace su::app {

enum class CoreError {
    kNullArgument,
    kInvalidUtf8,
    kUserDenied,
    kIoError,
    kParseError,
    kWriteError,
    kInvalidArgument,
    kBufferTooSmall,
    kUnknown,
};

struct CoreConfig {
    std::uint32_t version = 1;
    int selected_camera = 0;
    float recognition_threshold = 0.65F;
    bool liveness_detection = true;
    std::uint32_t preview_fps = 15;
};

struct AuthDecision {
    bool accepted = false;
};

struct FaceAuthDecision {
    bool accepted = false;
    float score = 0.0F;
    std::uint32_t profile_count = 0;
};

std::uint32_t core_version_major();
std::expected<float, CoreError> default_threshold();
CoreConfig default_config();
std::expected<CoreConfig, CoreError> load_config(const std::string& path);
std::expected<void, CoreError> save_config(const std::string& path, const CoreConfig& config);
std::expected<AuthDecision, CoreError> evaluate_auth(
    std::string_view username,
    float similarity,
    float threshold,
    bool liveness_ok);
std::expected<void, CoreError> enroll_face_profile(
    const std::string& store_path,
    std::string_view label,
    std::string_view sample_seed);
std::expected<bool, CoreError> delete_face_profile(
    const std::string& store_path,
    std::string_view profile_id);
std::expected<std::string, CoreError> list_face_profiles_json(const std::string& store_path);
std::expected<FaceAuthDecision, CoreError> authenticate_face_sample(
    const std::string& store_path,
    std::string_view sample_seed,
    float threshold);
std::expected<std::string, CoreError> authenticate_face_sample_report_json(
    const std::string& store_path,
    std::string_view sample_seed,
    float threshold);

}  // namespace su::app
