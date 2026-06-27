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

}  // namespace su::app
