// Compute a face embedding from an image file and print it in the
// `embedding:...` source format the profile store accepts. Acceptance-test
// helper: lets a headless Windows VM enroll a face profile from a still
// photo (fed through a virtual camera) without driving the GUI preview.
//
// Usage: embed_probe <image-file> [--with-liveness]
// Exit codes: 2 usage, 3 model unavailable, 4 image load failure,
// 5 no face detected / extraction failure.

#include <cstdio>
#include <filesystem>
#include <span>
#include <string>

import su.recognizer.image;
import su.recognizer.service;
import su.recognizer.types;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <image-file> [--with-liveness]\n", argv[0]);
        return 2;
    }
    const auto path = std::filesystem::path{argv[1]};
    if (!std::filesystem::exists(path)) {
        std::fprintf(stderr, "no such file: %s\n", path.string().c_str());
        return 2;
    }
    const auto with_liveness = argc >= 3
        && std::string_view{argv[2]} == "--with-liveness";

    auto recognizer = su::recognizer::RecognizerService{};
    if (!recognizer.seetaface_available()) {
        std::fprintf(stderr, "seetaface backend unavailable\n");
        return 3;
    }
    const auto loaded = su::recognizer::load_image_file(path);
    if (!loaded) {
        std::fprintf(stderr, "image load failed\n");
        return 4;
    }
    std::fprintf(
        stderr, "image %dx%d channels=%d\n", loaded->width, loaded->height,
        loaded->channels);
    const auto view = su::recognizer::ImageView{
        .width = loaded->width,
        .height = loaded->height,
        .channels = loaded->channels,
        .bytes = std::span<const std::byte>(loaded->bytes),
    };
    const auto extracted = recognizer.extract_from_image(view, with_liveness);
    if (!extracted) {
        std::fprintf(stderr, "extraction error\n");
        return 5;
    }
    if (!extracted->has_face) {
        std::fprintf(stderr, "no face detected\n");
        return 5;
    }
    std::fprintf(
        stderr, "face: yes, liveness=%f, dims=%zu\n",
        static_cast<double>(extracted->liveness_score), extracted->feature.size());
    const auto source = su::recognizer::embedding_sample_source(extracted->feature);
    std::printf("%.*s\n", static_cast<int>(source.size()), source.data());
    return 0;
}
