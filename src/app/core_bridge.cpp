#include "app/core_bridge.h"

#include "su_core.h"

namespace su::app {

namespace {

CoreError map_status(SuStatus status) {
    switch (status) {
    case SuStatus_Ok:
        return CoreError::kUnknown;
    case SuStatus_NullArgument:
        return CoreError::kNullArgument;
    case SuStatus_InvalidUtf8:
        return CoreError::kInvalidUtf8;
    case SuStatus_UserDenied:
        return CoreError::kUserDenied;
    case SuStatus_IoError:
        return CoreError::kIoError;
    case SuStatus_ParseError:
        return CoreError::kParseError;
    case SuStatus_WriteError:
        return CoreError::kWriteError;
    }
    return CoreError::kUnknown;
}

CoreConfig map_config(const SuCoreConfig& config) {
    return CoreConfig{
        .version = config.version,
        .selected_camera = config.selected_camera,
        .recognition_threshold = config.recognition_threshold,
        .liveness_detection = config.liveness_detection,
        .preview_fps = config.preview_fps,
    };
}

SuCoreConfig map_config(const CoreConfig& config) {
    return SuCoreConfig{
        .version = config.version,
        .selected_camera = config.selected_camera,
        .recognition_threshold = config.recognition_threshold,
        .liveness_detection = config.liveness_detection,
        .preview_fps = config.preview_fps,
    };
}

}  // namespace

std::uint32_t core_version_major() {
    return su_core_version_major();
}

std::expected<float, CoreError> default_threshold() {
    float threshold = 0.0F;
    const auto status = su_core_default_threshold(&threshold);
    if (status != SuStatus_Ok) {
        return std::unexpected(map_status(status));
    }
    return threshold;
}

CoreConfig default_config() {
    return map_config(su_core_default_config());
}

std::expected<CoreConfig, CoreError> load_config(const std::string& path) {
    SuCoreConfig config{};
    const auto status = su_core_load_config(path.c_str(), &config);
    if (status != SuStatus_Ok) {
        return std::unexpected(map_status(status));
    }
    return map_config(config);
}

std::expected<void, CoreError> save_config(const std::string& path, const CoreConfig& config) {
    const auto ffi_config = map_config(config);
    const auto status = su_core_save_config(path.c_str(), &ffi_config);
    if (status != SuStatus_Ok) {
        return std::unexpected(map_status(status));
    }
    return {};
}

std::expected<AuthDecision, CoreError> evaluate_auth(
    std::string_view username,
    float similarity,
    float threshold,
    bool liveness_ok) {
    const auto owned_username = std::string(username);
    const auto decision = su_core_evaluate_auth(
        owned_username.c_str(),
        similarity,
        threshold,
        liveness_ok);
    if (decision.status != SuStatus_Ok) {
        return std::unexpected(map_status(decision.status));
    }
    return AuthDecision{.accepted = decision.accepted};
}

}  // namespace su::app
