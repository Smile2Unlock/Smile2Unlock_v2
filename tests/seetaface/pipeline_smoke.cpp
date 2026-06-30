#include "app/core_bridge.h"
#include "recognizer/recognizer_service.h"
#include "recognizer/seetaface_backend.h"
#include "recognizer/image/image_loader.h"

#include <cassert>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

struct TestImage {
    int width = 0;
    int height = 0;
    int channels = 3;
    std::vector<std::byte> bytes;
};

void skip_ppm_space_and_comments(std::istream& input) {
    while (input) {
        const auto next = input.peek();
        if (next == '#') {
            std::string comment;
            std::getline(input, comment);
            continue;
        }
        if (std::isspace(next) != 0) {
            input.get();
            continue;
        }
        break;
    }
}

int read_ppm_int(std::istream& input) {
    skip_ppm_space_and_comments(input);
    auto value = 0;
    input >> value;
    return value;
}

TestImage load_ppm_rgb(const std::filesystem::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    assert(input.is_open());

    auto magic = std::string{};
    input >> magic;
    assert(magic == "P6");

    const auto width = read_ppm_int(input);
    const auto height = read_ppm_int(input);
    const auto max_value = read_ppm_int(input);
    assert(width > 0);
    assert(height > 0);
    assert(max_value == 255);
    input.get();

    auto bytes = std::vector<std::byte>(static_cast<std::size_t>(width * height * 3));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(input.gcount() == static_cast<std::streamsize>(bytes.size()));

    return TestImage{
        .width = width,
        .height = height,
        .channels = 3,
        .bytes = std::move(bytes),
    };
}

su::recognizer::RecognitionResult extract_required(
    const su::recognizer::SeetaFaceBackend& backend,
    const TestImage& image,
    bool liveness_enabled) {
    const auto result = backend.extract(su::recognizer::ImageView{
        .width = image.width,
        .height = image.height,
        .channels = image.channels,
        .bytes = std::span<const std::byte>(image.bytes),
    }, liveness_enabled);
    assert(result.has_value());
    assert(result->has_face);
    assert(result->face_box.has_value());
    assert(!result->feature.empty());
    assert(std::isfinite(result->liveness_score));
    assert(result->liveness_score >= 0.0F);
    assert(result->liveness_score <= 1.0F);
    return *result;
}

}  // namespace

int main() {
    const auto model_paths = su::recognizer::seetaface_model_paths(
        su::recognizer::default_seetaface_model_dir());
    assert(model_paths.has_value());

    auto backend = su::recognizer::SeetaFaceBackend(*model_paths);
    assert(backend.available());
    assert(backend.liveness_available());

    const auto sample_dir = std::filesystem::path(SU_SEETAFACE_TEST_DATA_DIR);
    const auto image_a = load_ppm_rgb(sample_dir / "official_face_1.ppm");
    const auto first = extract_required(backend, image_a, /*liveness_enabled=*/true);
    const auto second = extract_required(backend, image_a, /*liveness_enabled=*/true);

    auto recognizer = su::recognizer::RecognizerService{};
    const auto self_score = recognizer.compare_features(first.feature, second.feature);
    assert(self_score.has_value());
    assert(*self_score > 0.99F);

    const auto store_path = (
        std::filesystem::temp_directory_path() / "su_seetaface_pipeline_profiles.json"
    ).string();
    std::filesystem::remove(store_path);

    const auto enrolled = su::app::enroll_face_profile(
        store_path,
        "SeetaFace Official Sample",
        su::recognizer::embedding_sample_source(first.feature));
    assert(enrolled.has_value());

    const auto report = su::app::authenticate_face_sample_report(
        store_path,
        su::recognizer::embedding_sample_source(second.feature),
        0.99F);
    assert(report.has_value());
    assert(report->accepted);
    assert(report->score > 0.99F);
    assert(report->profile_count == 1);

    if (std::filesystem::exists(sample_dir / "official_face_2.ppm")) {
        const auto image_b = load_ppm_rgb(sample_dir / "official_face_2.ppm");
        const auto other = backend.extract(su::recognizer::ImageView{
            .width = image_b.width,
            .height = image_b.height,
            .channels = image_b.channels,
            .bytes = std::span<const std::byte>(image_b.bytes),
        }, /*liveness_enabled=*/true);
        if (other.has_value() && other->has_face) {
            assert(!other->feature.empty());
            assert(std::isfinite(other->liveness_score));
        }
    }

    // Exercise the real app image path: CImg-decoded PNG -> RecognizerService
    // (lazy SeetaFace backend) -> embedding: source -> Rust enroll/auth, with
    // the store-level dimension lock enforced end to end.
    if (std::filesystem::exists(sample_dir / "official_face_1.png")) {
        auto recognizer = su::recognizer::RecognizerService{};
        assert(recognizer.seetaface_available());

        auto loaded = su::recognizer::load_image_file(sample_dir / "official_face_1.png");
        assert(loaded.has_value());
        assert(loaded->width > 0);
        assert(loaded->height > 0);
        assert(loaded->channels == 3);

        auto extracted = recognizer.extract_from_image(su::recognizer::ImageView{
            .width = loaded->width,
            .height = loaded->height,
            .channels = loaded->channels,
            .bytes = std::span<const std::byte>(loaded->bytes),
        });
        assert(extracted.has_value());
        assert(extracted->has_face);
        assert(!extracted->feature.empty());

        const auto image_store_path = (
            std::filesystem::temp_directory_path() / "su_seetaface_image_pipeline_profiles.json"
        ).string();
        std::filesystem::remove(image_store_path);

        const auto png_embedding_source =
            su::recognizer::embedding_sample_source(extracted->feature);
        const auto enrolled = su::app::enroll_face_profile(
            image_store_path,
            "SeetaFace PNG Sample",
            png_embedding_source);
        assert(enrolled.has_value());

        // Re-extract from the same image and authenticate; the dimension must
        // match the locked store dimension and the self-match must pass.
        const auto re_extracted = recognizer.extract_from_image(su::recognizer::ImageView{
            .width = loaded->width,
            .height = loaded->height,
            .channels = loaded->channels,
            .bytes = std::span<const std::byte>(loaded->bytes),
        });
        assert(re_extracted.has_value());
        const auto probe_source =
            su::recognizer::embedding_sample_source(re_extracted->feature);
        const auto image_report = su::app::authenticate_face_sample_report(
            image_store_path,
            probe_source,
            0.99F);
        assert(image_report.has_value());
        assert(image_report->accepted);
        assert(image_report->score > 0.99F);
        assert(image_report->profile_count == 1);

        std::filesystem::remove(image_store_path);
    }

    std::filesystem::remove(store_path);
    return 0;
}
