module;
#include <cctype>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#if defined(_WIN32)
#include "platform/windows/paths.h"
#endif

module su.recognizer.backend;
import su.recognizer.types;

namespace su::recognizer {

namespace {

constexpr bool compiled_in(const BackendKind kind) {
    switch (kind) {
    case BackendKind::kLightFace: return SU_HAS_LIGHTFACE != 0;
    case BackendKind::kSeetaFace: return SU_HAS_SEETAFACE != 0;
    }
    return false;
}

constexpr BackendKind build_default_kind() {
    return compiled_in(BackendKind::kLightFace) ? BackendKind::kLightFace : BackendKind::kSeetaFace;
}

std::string requested_backend() {
#if defined(_WIN32)
    const auto value = su::windows::environment_path(L"SU_RECOGNIZER_BACKEND");
    return value ? value->string() : std::string{};
#else
    const auto* value = std::getenv("SU_RECOGNIZER_BACKEND");
    return value == nullptr ? std::string{} : std::string{value};
#endif
}

}  // namespace

BackendKind active_default_backend_kind() {
    auto requested = requested_backend();
    for (auto& ch : requested) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    // A request for a backend this build does not contain falls back to the
    // build default instead of leaving the product without recognition.
    if (requested == "seetaface" && compiled_in(BackendKind::kSeetaFace)) {
        return BackendKind::kSeetaFace;
    }
    if (requested == "lightface" && compiled_in(BackendKind::kLightFace)) {
        return BackendKind::kLightFace;
    }
    return build_default_kind();
}

std::filesystem::path default_model_dir() {
    return active_default_backend_kind() == BackendKind::kSeetaFace
        ? default_seetaface_model_dir()
        : default_lightface_model_dir();
}

std::expected<std::unique_ptr<FaceBackend>, RecognizerError> create_backend(const BackendKind kind) {
    try {
        switch (kind) {
        case BackendKind::kLightFace: {
            if (!compiled_in(BackendKind::kLightFace)) {
                break;
            }
            auto paths = lightface_model_paths(default_lightface_model_dir());
            if (!paths) {
                return std::unexpected(paths.error());
            }
            auto backend = std::make_unique<LightFaceBackend>(*std::move(paths));
            if (!backend->available()) {
                return std::unexpected(RecognizerError::kModelUnavailable);
            }
            return backend;
        }
        case BackendKind::kSeetaFace: {
            if (!compiled_in(BackendKind::kSeetaFace)) {
                break;
            }
            auto paths = seetaface_model_paths(default_seetaface_model_dir());
            if (!paths) {
                return std::unexpected(paths.error());
            }
            auto backend = std::make_unique<SeetaFaceBackend>(*std::move(paths));
            if (!backend->available()) {
                return std::unexpected(RecognizerError::kModelUnavailable);
            }
            return backend;
        }
        }
    } catch (...) {
        // Model constructors are third-party code; surface a safe unavailable
        // result and let the caller's bounded retry recover later.
    }
    return std::unexpected(RecognizerError::kModelUnavailable);
}

std::expected<std::unique_ptr<FaceBackend>, RecognizerError> create_default_backend() {
    return create_backend(active_default_backend_kind());
}

}  // namespace su::recognizer
