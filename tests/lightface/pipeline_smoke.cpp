#ifdef NDEBUG
#undef NDEBUG
#endif

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

import su.core.types;
import su.recognizer.types;
import su.recognizer.backend;
import su.recognizer.image;
import su.recognizer.service;

namespace {

std::vector<std::byte> make_blank_rgb(int w, int h, std::uint8_t value = 0) {
    return std::vector<std::byte>(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3U,
        static_cast<std::byte>(value));
}

}  // namespace

int main() {
    const auto model_dir = su::recognizer::default_lightface_model_dir();
    const auto model_paths = su::recognizer::lightface_model_paths(model_dir);
    assert(model_paths.has_value());

    assert(su::recognizer::active_default_backend_kind() == su::recognizer::BackendKind::kLightFace);

    auto backend = su::recognizer::LightFaceBackend(*model_paths);
    assert(backend.kind() == su::recognizer::BackendKind::kLightFace);
    assert(backend.name() == "lightface");
    assert(backend.embedding_dim() == 512);
    assert(backend.available());
    assert(backend.liveness_available());

    // Factory & RecognizerService integration.
    {
        auto created = su::recognizer::create_default_backend();
        assert(created.has_value());
        assert((*created)->kind() == su::recognizer::BackendKind::kLightFace);
        assert((*created)->embedding_dim() == 512);

        auto service = su::recognizer::RecognizerService{};
        assert(service.backend_available());
        assert(service.seetaface_available());
        assert(service.backend_name() == "lightface");
    }

    // Invalid image checks.
    {
        const auto empty_res = backend.extract(su::recognizer::ImageView{}, true);
        assert(!empty_res.has_value());
        assert(empty_res.error() == su::recognizer::RecognizerError::kInvalidImage);

        const auto tiny = std::array<std::byte, 10>{};
        const auto truncated = backend.extract(su::recognizer::ImageView{
            .width = 64, .height = 64, .channels = 3, .bytes = tiny}, true);
        assert(!truncated.has_value());
        assert(truncated.error() == su::recognizer::RecognizerError::kInvalidImage);
    }

    // Blank image -> no face, liveness 0.
    {
        const auto blank = make_blank_rgb(320, 240, 0);
        const auto res = backend.extract(su::recognizer::ImageView{
            .width = 320, .height = 240, .channels = 3, .bytes = blank}, true);
        assert(res.has_value());
        assert(!res->has_face);
        assert(!res->face_box.has_value());
        assert(res->feature.empty());
        assert(res->liveness_score == 0.0F);
    }

    // Missing FAS models degrade liveness cleanly without breaking extraction.
    {
        auto no_fas_paths = *model_paths;
        no_fas_paths.fas_spatial.clear();
        no_fas_paths.fas_temporal.clear();
        auto no_fas = su::recognizer::LightFaceBackend(no_fas_paths);
        assert(no_fas.available());
        assert(!no_fas.liveness_available());

        const auto blank = make_blank_rgb(320, 240, 128);
        const auto view = su::recognizer::ImageView{
            .width = 320, .height = 240, .channels = 3, .bytes = blank};
        const auto req_live = no_fas.extract(view, /*liveness_enabled=*/true);
        assert(!req_live.has_value());
        assert(req_live.error() == su::recognizer::RecognizerError::kModelUnavailable);

        const auto no_live = no_fas.extract(view, /*liveness_enabled=*/false);
        assert(no_live.has_value());
    }

    // Optional real-face end-to-end verification when SU_LIGHTFACE_TEST_IMAGE is set.
    if (const auto* test_image_env = std::getenv("SU_LIGHTFACE_TEST_IMAGE");
        test_image_env != nullptr && *test_image_env != '\0') {
        const auto loaded = su::recognizer::load_image_file(std::filesystem::path(test_image_env));
        assert(loaded.has_value());
        const auto view = su::recognizer::ImageView{
            .width = loaded->width, .height = loaded->height, .channels = loaded->channels,
            .bytes = loaded->bytes};

        const auto first = backend.extract(view, /*liveness_enabled=*/false);
        assert(first.has_value());
        assert(first->has_face);
        assert(first->face_box.has_value());
        assert(first->feature.size() == 512);
        assert(first->liveness_score == 1.0F);

        double norm_sq = 0.0;
        for (float v : first->feature) {
            assert(std::isfinite(v));
            norm_sq += static_cast<double>(v) * v;
        }
        assert(std::abs(std::sqrt(norm_sq) - 1.0) < 1e-3);

        auto service = su::recognizer::RecognizerService{};
        const auto second = service.extract_from_image(view, /*liveness_enabled=*/false);
        assert(second.has_value() && second->has_face);
        const auto self_cos = service.compare_features(first->feature, second->feature);
        assert(self_cos.has_value() && *self_cos > 0.999F);

        // RGBA / Gray channel invariance.
        std::vector<std::byte> rgba_bytes;
        std::vector<std::byte> gray_bytes;
        std::vector<std::byte> gray_rgb_bytes;
        rgba_bytes.reserve(static_cast<std::size_t>(loaded->width) * loaded->height * 4U);
        gray_bytes.reserve(static_cast<std::size_t>(loaded->width) * loaded->height);
        gray_rgb_bytes.reserve(loaded->bytes.size());
        for (std::size_t i = 0; i < loaded->bytes.size(); i += 3) {
            rgba_bytes.insert(rgba_bytes.end(), loaded->bytes.begin() + i, loaded->bytes.begin() + i + 3);
            rgba_bytes.push_back(std::byte{33});
            gray_bytes.push_back(loaded->bytes[i + 1]);
            gray_rgb_bytes.insert(gray_rgb_bytes.end(), 3, loaded->bytes[i + 1]);
        }
        const auto rgba_res = backend.extract(
            {loaded->width, loaded->height, 4, rgba_bytes}, false);
        assert(rgba_res.has_value() && rgba_res->has_face);
        const auto rgba_cos = service.compare_features(first->feature, rgba_res->feature);
        assert(rgba_cos.has_value() && *rgba_cos > 0.999F);

        const auto gray_res = backend.extract(
            {loaded->width, loaded->height, 1, gray_bytes}, false);
        const auto gray_rgb_res = backend.extract(
            {loaded->width, loaded->height, 3, gray_rgb_bytes}, false);
        assert(gray_res.has_value() && gray_res->has_face);
        assert(gray_rgb_res.has_value() && gray_rgb_res->has_face);
        const auto gray_cos = service.compare_features(gray_res->feature, gray_rgb_res->feature);
        assert(gray_cos.has_value() && *gray_cos > 0.999F);

        // Multi-face rejection (side-by-side duplicate).
        std::vector<std::byte> multi_bytes;
        const auto row_bytes = static_cast<std::size_t>(loaded->width) * 3U;
        for (int y = 0; y < loaded->height; ++y) {
            const auto row = loaded->bytes.begin() + static_cast<std::ptrdiff_t>(y * row_bytes);
            multi_bytes.insert(multi_bytes.end(), row, row + static_cast<std::ptrdiff_t>(row_bytes));
            multi_bytes.insert(multi_bytes.end(), row, row + static_cast<std::ptrdiff_t>(row_bytes));
        }
        const auto multi_res = backend.extract(
            {loaded->width * 2, loaded->height, 3, multi_bytes}, false);
        assert(multi_res.has_value() && !multi_res->has_face && multi_res->feature.empty());

        // 8-frame streaming liveness warmup & reset.
        backend.reset_liveness();
        for (int i = 0; i < 7; ++i) {
            const auto warm = backend.predict_liveness(view, /*liveness_enabled=*/true);
            assert(warm.has_value() && warm->has_face);
            assert(warm->liveness_score == 0.0F);
        }
        const auto frame8 = backend.predict_liveness(view, /*liveness_enabled=*/true);
        assert(frame8.has_value() && frame8->has_face);
        assert(std::isfinite(frame8->liveness_score));
        assert(frame8->liveness_score >= 0.0F && frame8->liveness_score <= 1.0F);
        backend.reset_liveness();
        const auto after_reset = backend.predict_liveness(view, /*liveness_enabled=*/true);
        assert(after_reset.has_value() && after_reset->liveness_score == 0.0F);

        // End-to-end Rust profile store round-trip with 512-d embedding.
        const auto store_path = (
            std::filesystem::temp_directory_path() / "su_lightface_pipeline_profiles.json"
        ).string();
        std::filesystem::remove(store_path);
        const auto guard = std::shared_ptr<void>(nullptr,
            [store_path](...) { std::filesystem::remove(store_path); });
        const auto enrolled = su::app::enroll_face_profile(
            store_path, "LightFace Sample",
            su::recognizer::embedding_sample_source(first->feature));
        assert(enrolled.has_value());
        const auto report = su::app::authenticate_face_sample_report(
            store_path,
            su::recognizer::embedding_sample_source(second->feature),
            0.40F);
        assert(report.has_value() && report->accepted && report->score > 0.999F);
        std::cout << "PASS: LightFace real-face E2E test (512-d, liveness_8f="
                  << frame8->liveness_score << ")\n";
    }

    std::cout << "PASS: LightFace pipeline smoke tests\n";
    return 0;
}
