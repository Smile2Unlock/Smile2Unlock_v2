#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <limits>
#include "recognizer/liveness_window.h"
#include <seeta/FaceAntiSpoofing.h>
#include <seeta/FaceDetector.h>
#include <seeta/FaceLandmarker.h>
#include <seeta/FaceRecognizer.h>

import su.core.types;
import su.recognizer.types;
import su.recognizer.backend;
import su.recognizer.image;
import su.recognizer.service;

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
    if (!(input >> value)) {
        // Malformed input: return 0 so the caller's assert(width > 0) catches
        // it in debug builds, avoiding silent data corruption in release.
        input.clear();
    }
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
    (void)max_value;
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
    auto window = su::recognizer::detail::LivenessWindow{};
    // Eleven 0.75 scores must never pass the 0.8 gate (upstream computes
    // 11 * 0.75 / 10). Keep checking after the ring wraps several times.
    for (auto i = 0; i < 30; ++i) {
        assert(window.push(1.0F, 0.75F) == 0.0F);
    }
    window.reset();
    for (auto i = 0; i < 9; ++i) {
        assert(window.push(1.0F, 0.9F) == 0.0F);
    }
    assert(window.push(1.0F, 0.9F) == 0.9F);
    assert(window.push(0.2F, 0.99F) == 0.0F);
    assert(window.push(1.0F, 0.9F) == 0.0F);
    assert(window.push(1.0F, std::numeric_limits<float>::quiet_NaN()) == 0.0F);
    assert(window.push(1.0F, std::numeric_limits<float>::infinity()) == 0.0F);

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

    // Compare our RGB boundary with the SDK's documented BGR input, using
    // the same face box. Self-comparison alone cannot catch swapped channels.
    const auto sdk_reference = [&](const TestImage& reference_image,
                                   const su::recognizer::RecognitionResult& reference_result) {
        auto bgr = std::vector<unsigned char>(reference_image.bytes.size());
        for (std::size_t i = 0; i < bgr.size(); i += 3) {
            bgr[i] = std::to_integer<unsigned char>(reference_image.bytes[i + 2]);
            bgr[i + 1] = std::to_integer<unsigned char>(reference_image.bytes[i + 1]);
            bgr[i + 2] = std::to_integer<unsigned char>(reference_image.bytes[i]);
        }
        const auto sdk_image = SeetaImageData{reference_image.width, reference_image.height, 3, bgr.data()};
        const auto box = *reference_result.face_box;
        const auto sdk_face = SeetaRect{box.x, box.y, box.width, box.height};
        auto landmark_setting = seeta::ModelSetting{};
        landmark_setting.append(model_paths->landmarker.string());
        auto sdk_landmarker = seeta::FaceLandmarker(landmark_setting);
        const auto sdk_points = sdk_landmarker.mark(sdk_image, sdk_face);
        auto feature_setting = seeta::ModelSetting{};
        feature_setting.append(model_paths->recognizer.string());
        auto sdk_recognizer = seeta::FaceRecognizer(feature_setting);
        auto sdk_feature = std::vector<float>(sdk_recognizer.GetExtractFeatureSize());
        assert(sdk_recognizer.Extract(sdk_image, sdk_points.data(), sdk_feature.data()));
        auto sdk_fas_setting = seeta::ModelSetting{};
        sdk_fas_setting.append(model_paths->anti_spoofing_first.string());
        sdk_fas_setting.append(model_paths->anti_spoofing_second.string());
        auto sdk_fas = seeta::FaceAntiSpoofing(sdk_fas_setting);
        sdk_fas.SetThreshold(0.3F, 0.8F);
        (void)sdk_fas.Predict(sdk_image, sdk_face, sdk_points.data());
        float sdk_clarity = 0.0F, sdk_reality = 0.0F;
        sdk_fas.GetPreFrameScore(&sdk_clarity, &sdk_reality);
        auto expected_window = su::recognizer::detail::LivenessWindow{};
        auto final_score = 0.0F;
        backend.reset_liveness();
        for (auto i = 0; i < 10; ++i) {
            const auto predicted = backend.predict_liveness(su::recognizer::ImageView{
                reference_image.width, reference_image.height, 3, reference_image.bytes}, true);
            assert(predicted && predicted->has_face);
            final_score = predicted->liveness_score;
            assert(std::abs(predicted->liveness_score
                - expected_window.push(sdk_clarity, sdk_reality)) < 1e-4F);
        }
        std::cout << "PASS: SDK BGR anti-spoofing agreement clarity=" << sdk_clarity
                  << " reality=" << sdk_reality << " ten-frame score=" << final_score << '\n';
        return sdk_feature;
    };
    const auto sdk_feature = sdk_reference(image_a, first);

    // Verify that extract with liveness_enabled=false skips the anti-spoofing
    // Predict and returns liveness_score=1.0 (passing) with a valid face box.
    const auto no_liveness = backend.extract(su::recognizer::ImageView{
        .width = image_a.width,
        .height = image_a.height,
        .channels = image_a.channels,
        .bytes = std::span<const std::byte>(image_a.bytes),
    }, /*liveness_enabled=*/false);
    assert(no_liveness.has_value());
    assert(no_liveness->has_face);
    assert(no_liveness->liveness_score == 1.0F);
    assert(std::isfinite(no_liveness->liveness_score));

    auto recognizer = su::recognizer::RecognizerService{};
    const auto sdk_agreement = recognizer.compare_features(first.feature, sdk_feature);
    assert(sdk_agreement && *sdk_agreement > 0.9999F);
    std::cout << "PASS: SDK BGR embedding agreement cosine=" << *sdk_agreement << '\n';
    const auto self_score = recognizer.compare_features(first.feature, second.feature);
    assert(self_score.has_value());
    assert(*self_score > 0.99F);

    // Channel normalization must preserve embeddings, including alpha values
    // that are not opaque. Grayscale must agree with replicated RGB pixels.
    auto rgba = TestImage{image_a.width, image_a.height, 4, {}};
    auto gray = TestImage{image_a.width, image_a.height, 1, {}};
    auto gray_rgb = TestImage{image_a.width, image_a.height, 3, {}};
    for (auto i = std::size_t{}; i < image_a.bytes.size(); i += 3) {
        rgba.bytes.insert(rgba.bytes.end(), image_a.bytes.begin() + i,
                          image_a.bytes.begin() + i + 3);
        rgba.bytes.push_back(std::byte{17});
        gray.bytes.push_back(image_a.bytes[i + 1]);
        gray_rgb.bytes.insert(gray_rgb.bytes.end(), 3, image_a.bytes[i + 1]);
    }
    const auto rgba_result = extract_required(backend, rgba, false);
    const auto rgba_score = recognizer.compare_features(first.feature, rgba_result.feature);
    assert(rgba_score && *rgba_score > 0.999F);
    const auto gray_result = extract_required(backend, gray, false);
    const auto gray_rgb_result = extract_required(backend, gray_rgb, false);
    const auto gray_score = recognizer.compare_features(gray_result.feature, gray_rgb_result.feature);
    assert(gray_score && *gray_score > 0.999F);

    const auto view = [](const TestImage& image) {
        return su::recognizer::ImageView{image.width, image.height, image.channels, image.bytes};
    };
    const auto blank = TestImage{image_a.width, image_a.height, 3,
        std::vector<std::byte>(image_a.bytes.size())};
    const auto no_face = backend.predict_liveness(view(blank), true);
    assert(no_face && !no_face->has_face && no_face->liveness_score == 0.0F);
    // A new uninterrupted stream must start in DETECTING after a gap.
    const auto after_gap = backend.predict_liveness(view(image_a), true);
    assert(after_gap && after_gap->has_face && after_gap->liveness_score == 0.0F);
    auto invalid = view(image_a);
    invalid.bytes = invalid.bytes.first(10);
    const auto truncated = backend.extract(invalid, true);
    assert(!truncated && truncated.error() == su::recognizer::RecognizerError::kInvalidImage);
    const auto after_invalid = backend.predict_liveness(view(image_a), true);
    assert(after_invalid && after_invalid->liveness_score == 0.0F);

    auto multiple = TestImage{image_a.width * 2, image_a.height, 3, {}};
    const auto row_bytes = static_cast<std::size_t>(image_a.width) * 3;
    for (auto y = 0; y < image_a.height; ++y) {
        const auto row = image_a.bytes.begin() + y * row_bytes;
        multiple.bytes.insert(multiple.bytes.end(), row, row + row_bytes);
        multiple.bytes.insert(multiple.bytes.end(), row, row + row_bytes);
    }
    const auto multi_face = backend.extract(view(multiple), true);
    assert(multi_face && !multi_face->has_face && multi_face->feature.empty());

    // Backlit scene (#137): the face is darkened by `gain` and surrounded by
    // a bright window border, so the centre meter reads dark.
    const auto make_backlit = [&](const double gain) {
        auto backlit = TestImage{image_a.width, image_a.height, 3, image_a.bytes};
        const auto border_x = image_a.width / 10;
        const auto border_y = image_a.height / 10;
        for (auto y = 0; y < image_a.height; ++y) {
            for (auto x = 0; x < image_a.width; ++x) {
                const auto inside = x >= border_x && x < image_a.width - border_x
                    && y >= border_y && y < image_a.height - border_y;
                for (auto c = 0; c < 3; ++c) {
                    auto& value = backlit.bytes[(static_cast<std::size_t>(y) * image_a.width + x) * 3 + c];
                    value = inside
                        ? static_cast<std::byte>(std::to_integer<int>(value) * gain)
                        : std::byte{255};
                }
            }
        }
        return backlit;
    };
    // Guards the premise: without the low-light retry the plain detector
    // pass must miss the frame, so a face below proves the retry ran.
    auto detector_setting = seeta::ModelSetting{};
    detector_setting.append(model_paths->detector.string());
    auto raw_detector = seeta::FaceDetector(detector_setting);
    const auto raw_detector_finds_face = [&](const TestImage& frame) {
        auto bgr = std::vector<unsigned char>(frame.bytes.size());
        for (auto i = std::size_t{}; i < bgr.size(); i += 3) {
            bgr[i] = std::to_integer<unsigned char>(frame.bytes[i + 2]);
            bgr[i + 1] = std::to_integer<unsigned char>(frame.bytes[i + 1]);
            bgr[i + 2] = std::to_integer<unsigned char>(frame.bytes[i]);
        }
        return raw_detector.detect(SeetaImageData{frame.width, frame.height, 3, bgr.data()}).size > 0;
    };
    // The plain pass still finds a face at this gain, so it is a normal
    // result regardless of how dark the face is: the floor only guards
    // enhanced-only detections.
    const auto plain_dark = make_backlit(0.08);
    assert(raw_detector_finds_face(plain_dark));
    const auto plain_dark_result = backend.extract(view(plain_dark), false);
    assert(plain_dark_result && plain_dark_result->has_face);
    assert(plain_dark_result->feature.size() == first.feature.size());

    // At these gains only the enhanced pass finds the face, and the original
    // face mean is far below the floor, so it is metering-only: a box but no
    // landmarks, liveness or features. (With this detector no gain exists
    // where the plain pass misses yet the face mean stays at or above the
    // floor: the plain pass already finds faces down to a mean of ~9. The
    // at-floor decision is covered by enhanced_face_is_usable in
    // su_low_light_enhance_test.)
    for (const auto gain : {0.06, 0.04}) {
        const auto backlit = make_backlit(gain);
        assert(!raw_detector_finds_face(backlit));
        const auto backlit_result = backend.extract(view(backlit), true);
        assert(backlit_result && !backlit_result->has_face);
        assert(backlit_result->face_box.has_value());
        assert(backlit_result->face_box->width > 0 && backlit_result->face_box->height > 0);
        assert(backlit_result->face_box->x >= 0 && backlit_result->face_box->y >= 0);
        assert(backlit_result->face_box->x + backlit_result->face_box->width <= backlit.width);
        assert(backlit_result->face_box->y + backlit_result->face_box->height <= backlit.height);
        assert(backlit_result->feature.empty());
        assert(backlit_result->liveness_score == 0.0F);
        // The liveness window was reset exactly like the no-face path.
        const auto after_metering = backend.predict_liveness(view(image_a), true);
        assert(after_metering && after_metering->has_face && after_metering->liveness_score == 0.0F);
    }
    std::cout << "PASS: dark backlit face is metering-only\n";

    // Switch off (#137): the first detector pass is final, so the frames that
    // only the retry could find yield no face and no box at all.
    for (const auto gain : {0.06, 0.04}) {
        const auto backlit = make_backlit(gain);
        const auto no_retry = backend.extract(view(backlit), true, /*low_light_retry=*/false);
        assert(no_retry && !no_retry->has_face && !no_retry->face_box.has_value());
        const auto no_retry_liveness =
            backend.predict_liveness(view(backlit), true, /*low_light_retry=*/false);
        assert(no_retry_liveness && !no_retry_liveness->face_box.has_value());
        // Ordinary frames are unaffected by the switch.
        const auto plain = backend.extract(view(image_a), true, /*low_light_retry=*/false);
        assert(plain && plain->has_face && !plain->feature.empty());
    }
    {
        auto service = su::recognizer::RecognizerService{};
        assert(service.backlight_compensation());
        const auto backlit = make_backlit(0.04);
        const auto with_retry = service.extract_from_image(view(backlit), true);
        assert(with_retry && with_retry->face_box.has_value());
        service.set_backlight_compensation(false);
        assert(!service.backlight_compensation());
        const auto without_retry = service.extract_from_image(view(backlit), true);
        assert(without_retry && !without_retry->has_face && !without_retry->face_box.has_value());
        // Off, or without a camera, the exposure loop never reports a change.
        assert(!service.observe_exposure(view(backlit), std::nullopt, 0));
        service.set_backlight_compensation(true);
        assert(service.extract_from_image(view(backlit), true)->face_box.has_value());
    }
    std::cout << "PASS: backlight switch off disables the low-light retry\n";

    auto missing_fas_paths = *model_paths;
    missing_fas_paths.anti_spoofing_first.clear();
    const auto missing_fas = su::recognizer::SeetaFaceBackend(missing_fas_paths);
    assert(missing_fas.available() && !missing_fas.liveness_available());
    const auto requires_fas = missing_fas.extract(view(image_a), true);
    assert(!requires_fas && requires_fas.error() == su::recognizer::RecognizerError::kModelUnavailable);
    const auto skips_fas = missing_fas.extract(view(image_a), false);
    assert(skips_fas && skips_fas->has_face && skips_fas->liveness_score == 1.0F);

    // RAII scope guard: clean up the temp profile store on every exit path.
    const auto store_path = (
        std::filesystem::temp_directory_path() / "su_seetaface_pipeline_profiles.json"
    ).string();
    std::filesystem::remove(store_path);
    const auto store_guard = std::shared_ptr<void>(nullptr,
        [store_path](...) { std::filesystem::remove(store_path); });

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
            const auto other_sdk = sdk_reference(image_b, *other);
            const auto agreement = recognizer.compare_features(other->feature, other_sdk);
            assert(agreement && *agreement > 0.9999F);
        }
    }

    const auto jpeg = su::recognizer::load_image_file(sample_dir / "official_face_2.jpg");
    assert(jpeg && jpeg->width > 0 && jpeg->height > 0 && jpeg->channels == 3);

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
        const auto image_store_guard = std::shared_ptr<void>(nullptr,
            [image_store_path](...) { std::filesystem::remove(image_store_path); });

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

    }

    return 0;
}
