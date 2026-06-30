#include "app/core_bridge.h"
#include "recognizer/recognizer_service.h"
#include "recognizer/seetaface_backend.h"

#include <cassert>
#include <array>
#include <filesystem>

int main() {
    const auto threshold = su::app::default_threshold();
    assert(threshold.has_value());
    assert(*threshold > 0.0F);

    const auto decision = su::app::evaluate_auth("demo", 0.72F, *threshold, true);
    assert(decision.has_value());
    assert(decision->accepted);

    // Verify that evaluate_auth with liveness_ok=false rejects the request.
    const auto no_liveness_auth = su::app::evaluate_auth("demo", 0.72F, *threshold, false);
    assert(no_liveness_auth.has_value());
    assert(!no_liveness_auth->accepted);

    // RAII scope guard: ensure the temp profile store is cleaned up on every
    // exit path (including early assert failures that abort).
    const auto store_path = (
        std::filesystem::temp_directory_path() / "su_protocol_smoke_profiles.json"
    ).string();
    std::filesystem::remove(store_path);
    const auto store_guard = std::shared_ptr<void>(nullptr,
        [store_path](...) { std::filesystem::remove(store_path); });

    const auto feature = std::array{1.0F, 0.0F, 0.0F, 0.0F};
    const auto sample = su::recognizer::embedding_sample_source(feature);
    const auto enrolled = su::app::enroll_face_profile(store_path, "Smoke Face", sample);
    assert(enrolled.has_value());

    const auto face_decision = su::app::authenticate_face_sample(store_path, sample, 0.95F);
    assert(face_decision.has_value());
    assert(face_decision->accepted);

    const auto no_liveness_decision = su::app::authenticate_face_sample(
        store_path,
        sample,
        0.95F,
        false);
    assert(no_liveness_decision.has_value());
    assert(!no_liveness_decision->accepted);
    assert(no_liveness_decision->score > 0.99F);

    const auto no_liveness_report = su::app::authenticate_face_sample_report(
        store_path,
        sample,
        0.95F,
        false);
    assert(no_liveness_report.has_value());
    assert(!no_liveness_report->accepted);
    assert(!no_liveness_report->liveness_ok);
    assert(no_liveness_report->reason == "liveness check failed");

#if SU_HAS_SEETAFACE
    const auto model_paths = su::recognizer::seetaface_model_paths(
        su::recognizer::default_seetaface_model_dir());
    assert(model_paths.has_value());
    auto backend = su::recognizer::SeetaFaceBackend(*model_paths);
    const auto empty_extract = backend.extract(su::recognizer::ImageView{}, /*liveness_enabled=*/true);
    assert(!empty_extract.has_value());
    if (backend.available()) {
        assert(empty_extract.error() == su::recognizer::RecognizerError::kInvalidImage);
    } else {
        assert(empty_extract.error() == su::recognizer::RecognizerError::kModelUnavailable);
    }
#endif

    return 0;
}
