module;
#include "su_core.h"

module su.core.types;
import std;

namespace su::app {

namespace {

CoreError map_status(SuStatus status) {
    switch (status) {
    case SuStatus_Ok:
        // Callers check Ok before mapping; reaching here is a programming
        // error. Surface it distinctly rather than silently mapping to Unknown.
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
        .liveness_threshold = config.liveness_threshold,
        .preview_fps = config.preview_fps,
    };
}

SuCoreConfig map_config(const CoreConfig& config) {
    return SuCoreConfig{
        .version = config.version,
        .selected_camera = config.selected_camera,
        .recognition_threshold = config.recognition_threshold,
        .liveness_detection = config.liveness_detection,
        .liveness_threshold = config.liveness_threshold,
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

std::string fixed_string(const std::uint8_t* data, std::size_t capacity) {
    const auto* begin = reinterpret_cast<const char*>(data);
    const auto* end = std::find(begin, begin + capacity, '\0');
    return std::string(begin, end);
}

FaceProfileSummary map_profile_summary(const SuFaceProfileSummary& profile) {
    return FaceProfileSummary{
        .id = fixed_string(profile.id, SuFaceProfileIdCap),
        .label = fixed_string(profile.label, SuFaceProfileLabelCap),
        .created_at_unix = profile.created_at_unix,
    };
}

FaceAuthReport map_auth_report(const SuFaceAuthReport& report) {
    return FaceAuthReport{
        .accepted = report.accepted,
        .score = report.score,
        .threshold = report.threshold,
        .liveness_ok = report.liveness_ok,
        .profile_count = report.profile_count,
        .best_profile_id = fixed_string(report.best_profile_id, SuFaceProfileIdCap),
        .best_profile_label = fixed_string(report.best_profile_label, SuFaceProfileLabelCap),
        .reason = fixed_string(report.reason, SuFaceAuthReasonCap),
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

std::expected<void, CoreError> enroll_face_profile(
    const std::string& store_path,
    std::string_view label,
    std::string_view face_sample_source) {
    const auto owned_label = std::string(label);
    const auto owned_face_sample_source = std::string(face_sample_source);
    const auto status = su_core_enroll_face_profile(
        store_path.c_str(),
        owned_label.c_str(),
        owned_face_sample_source.c_str());
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

std::expected<std::vector<FaceProfileSummary>, CoreError> list_face_profile_summaries(
    const std::string& store_path) {
    std::uintptr_t count = 0;
    auto status = su_core_list_face_profile_summaries(
        store_path.c_str(),
        nullptr,
        0,
        &count);
    if (status != SuStatus_BufferTooSmall && status != SuStatus_Ok) {
        return std::unexpected(map_status(status));
    }
    if (count == 0) {
        return std::vector<FaceProfileSummary>{};
    }

    std::vector<SuFaceProfileSummary> ffi_profiles(count);
    status = su_core_list_face_profile_summaries(
        store_path.c_str(),
        ffi_profiles.data(),
        ffi_profiles.size(),
        &count);
    if (status != SuStatus_Ok) {
        return std::unexpected(map_status(status));
    }

    auto profiles = std::vector<FaceProfileSummary>(count);
    std::ranges::transform(ffi_profiles, profiles.begin(), map_profile_summary);
    return profiles;
}

std::expected<FaceAuthDecision, CoreError> authenticate_face_sample(
    const std::string& store_path,
    std::string_view face_sample_source,
    float threshold) {
    return authenticate_face_sample(store_path, face_sample_source, threshold, true);
}

std::expected<FaceAuthDecision, CoreError> authenticate_face_sample(
    const std::string& store_path,
    std::string_view face_sample_source,
    float threshold,
    bool liveness_ok) {
    const auto owned_face_sample_source = std::string(face_sample_source);
    const auto decision = su_core_authenticate_face_sample_with_liveness(
        store_path.c_str(),
        owned_face_sample_source.c_str(),
        threshold,
        liveness_ok);
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
    std::string_view face_sample_source,
    float threshold) {
    return authenticate_face_sample_report_json(store_path, face_sample_source, threshold, true);
}

std::expected<std::string, CoreError> authenticate_face_sample_report_json(
    const std::string& store_path,
    std::string_view face_sample_source,
    float threshold,
    bool liveness_ok) {
    const auto owned_face_sample_source = std::string(face_sample_source);
    return read_json_from_core([&store_path, &owned_face_sample_source, threshold, liveness_ok](
                                   std::uint8_t* buffer,
                                   std::uintptr_t buffer_len,
                                   std::uintptr_t* required_len) {
        return su_core_authenticate_face_sample_report_json_with_liveness(
            store_path.c_str(),
            owned_face_sample_source.c_str(),
            threshold,
            liveness_ok,
            buffer,
            buffer_len,
            required_len);
    });
}

std::expected<FaceAuthReport, CoreError> authenticate_face_sample_report(
    const std::string& store_path,
    std::string_view face_sample_source,
    float threshold) {
    return authenticate_face_sample_report(store_path, face_sample_source, threshold, true);
}

std::expected<FaceAuthReport, CoreError> authenticate_face_sample_report(
    const std::string& store_path,
    std::string_view face_sample_source,
    float threshold,
    bool liveness_ok) {
    const auto owned_face_sample_source = std::string(face_sample_source);
    const auto report = su_core_authenticate_face_sample_report_with_liveness(
        store_path.c_str(),
        owned_face_sample_source.c_str(),
        threshold,
        liveness_ok);
    if (report.status != SuStatus_Ok) {
        return std::unexpected(map_status(report.status));
    }
    return map_auth_report(report);
}

}  // namespace su::app
