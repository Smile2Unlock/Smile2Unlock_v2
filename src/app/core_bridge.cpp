#include "app/core_bridge.h"

#include "su_core.h"

#include <vector>

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
    case SuStatus_InvalidArgument:
        return CoreError::kInvalidArgument;
    case SuStatus_BufferTooSmall:
        return CoreError::kBufferTooSmall;
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

std::expected<std::string, CoreError> read_json_from_core(
    const auto& producer) {
    std::uintptr_t required_len = 0;
    auto status = producer(nullptr, 0, &required_len);
    if (status != SuStatus_BufferTooSmall && status != SuStatus_Ok) {
        return std::unexpected(map_status(status));
    }
    if (required_len == 0) {
        return std::string{};
    }

    std::vector<std::uint8_t> buffer(required_len);
    status = producer(buffer.data(), buffer.size(), &required_len);
    if (status != SuStatus_Ok) {
        return std::unexpected(map_status(status));
    }
    return std::string(reinterpret_cast<const char*>(buffer.data()));
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

std::expected<void, CoreError> enroll_face_profile(
    const std::string& store_path,
    std::string_view label,
    std::string_view sample_seed) {
    const auto owned_label = std::string(label);
    const auto owned_sample_seed = std::string(sample_seed);
    const auto status = su_core_enroll_face_profile(
        store_path.c_str(),
        owned_label.c_str(),
        owned_sample_seed.c_str());
    if (status != SuStatus_Ok) {
        return std::unexpected(map_status(status));
    }
    return {};
}

std::expected<bool, CoreError> delete_face_profile(
    const std::string& store_path,
    std::string_view profile_id) {
    const auto owned_profile_id = std::string(profile_id);
    bool deleted = false;
    const auto status = su_core_delete_face_profile(
        store_path.c_str(),
        owned_profile_id.c_str(),
        &deleted);
    if (status != SuStatus_Ok) {
        return std::unexpected(map_status(status));
    }
    return deleted;
}

std::expected<std::string, CoreError> list_face_profiles_json(const std::string& store_path) {
    return read_json_from_core([&store_path](std::uint8_t* buffer, std::uintptr_t buffer_len, std::uintptr_t* required_len) {
        return su_core_list_face_profiles_json(
            store_path.c_str(),
            buffer,
            buffer_len,
            required_len);
    });
}

std::expected<FaceAuthDecision, CoreError> authenticate_face_sample(
    const std::string& store_path,
    std::string_view sample_seed,
    float threshold) {
    const auto owned_sample_seed = std::string(sample_seed);
    const auto decision = su_core_authenticate_face_sample(
        store_path.c_str(),
        owned_sample_seed.c_str(),
        threshold);
    if (decision.status != SuStatus_Ok) {
        return std::unexpected(map_status(decision.status));
    }
    return FaceAuthDecision{
        .accepted = decision.accepted,
        .score = decision.score,
        .profile_count = decision.profile_count,
    };
}

std::expected<std::string, CoreError> authenticate_face_sample_report_json(
    const std::string& store_path,
    std::string_view sample_seed,
    float threshold) {
    const auto owned_sample_seed = std::string(sample_seed);
    return read_json_from_core([&store_path, &owned_sample_seed, threshold](
                                   std::uint8_t* buffer,
                                   std::uintptr_t buffer_len,
                                   std::uintptr_t* required_len) {
        return su_core_authenticate_face_sample_report_json(
            store_path.c_str(),
            owned_sample_seed.c_str(),
            threshold,
            buffer,
            buffer_len,
            required_len);
    });
}

}  // namespace su::app
