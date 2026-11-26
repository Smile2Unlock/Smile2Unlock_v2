#ifdef NDEBUG
#undef NDEBUG
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "recognizer/lightface_preprocess.h"

namespace lf = su::recognizer::lightface;

namespace {

void test_letterbox_and_unletterbox() {
    // Landscape 1920x1080 -> 640x360, pad_x=0, pad_y=140.
    {
        auto rgb = std::vector<std::uint8_t>(1920U * 1080U * 3U, 200);
        const auto lb = lf::letterbox_rgb(rgb, 1920, 1080);
        assert(std::abs(lb.scale - (640.0F / 1920.0F)) < 1e-6F);
        assert(lb.pad_x == 0);
        assert(lb.pad_y == 140);
        assert(lb.rgb.size() == 640U * 640U * 3U);
        // Top padding bar must be 114, centre must be ~200.
        assert(lb.rgb[0] == lf::kDetPadValue);
        const auto mid = (320U * 640U + 320U) * 3U;
        assert(lb.rgb[mid] == 200 && lb.rgb[mid + 1] == 200 && lb.rgb[mid + 2] == 200);

        const auto det = lf::Detection{
            .x1 = 100.0F, .y1 = 140.0F, .x2 = 200.0F, .y2 = 240.0F, .score = 0.9F,
            .kpts = {{{120.0F, 170.0F}, {180.0F, 170.0F}, {150.0F, 195.0F},
                      {130.0F, 220.0F}, {170.0F, 220.0F}}},
        };
        const auto un = lf::unletterbox_detection(det, lb.scale, lb.pad_x, lb.pad_y, 1920, 1080);
        assert(std::abs(un.x1 - 300.0F) < 1e-3F);
        assert(std::abs(un.y1 - 0.0F) < 1e-3F);
        assert(std::abs(un.x2 - 600.0F) < 1e-3F);
        assert(std::abs(un.y2 - 300.0F) < 1e-3F);
        assert(std::abs(un.kpts[0][0] - 360.0F) < 1e-3F);
        assert(std::abs(un.kpts[0][1] - 90.0F) < 1e-3F);
    }

    // Portrait 480x640 -> 480x640 (scale=1.0), pad_x=80, pad_y=0.
    {
        auto rgb = std::vector<std::uint8_t>(480U * 640U * 3U, 42);
        const auto lb = lf::letterbox_rgb(rgb, 480, 640);
        assert(std::abs(lb.scale - 1.0F) < 1e-6F);
        assert(lb.pad_x == 80);
        assert(lb.pad_y == 0);
        assert(lb.rgb[0] == lf::kDetPadValue);
        const auto inner = (10U * 640U + 90U) * 3U;
        assert(lb.rgb[inner] == 42);
    }

    // Square 640x640 -> identity.
    {
        auto rgb = std::vector<std::uint8_t>(640U * 640U * 3U, 77);
        const auto lb = lf::letterbox_rgb(rgb, 640, 640);
        assert(std::abs(lb.scale - 1.0F) < 1e-6F);
        assert(lb.pad_x == 0 && lb.pad_y == 0);
        assert(lb.rgb[0] == 77);
    }
}

void test_decode_and_nms() {
    // 4 boxes:
    //  0: score 0.90 at [100,100,200,200]
    //  1: score 0.80 overlapping [105,105,205,205] -> suppressed
    //  2: score 0.70 separate    [300,300,400,400] -> kept
    //  3: score 0.30 below 0.45  [500,500,550,550] -> filtered out
    const auto boxes = std::array<float, 16>{
        100.0F, 100.0F, 200.0F, 200.0F,
        105.0F, 105.0F, 205.0F, 205.0F,
        300.0F, 300.0F, 400.0F, 400.0F,
        500.0F, 500.0F, 550.0F, 550.0F,
    };
    const auto scores = std::array<float, 4>{0.80F, 0.90F, 0.70F, 0.30F};
    // Make index 1 score 0.90 and index 0 score 0.80 so sorting is also tested.
    auto kpts = std::array<float, 40>{};
    kpts[1 * 10 + 0] = 150.0F;
    kpts[1 * 10 + 1] = 150.0F;
    const auto kept = lf::decode_and_nms(boxes, scores, kpts);
    assert(kept.size() == 2);
    assert(std::abs(kept[0].score - 0.90F) < 1e-6F);
    assert(std::abs(kept[0].x1 - 105.0F) < 1e-6F);
    assert(std::abs(kept[0].kpts[0][0] - 150.0F) < 1e-6F);
    assert(std::abs(kept[1].score - 0.70F) < 1e-6F);
}

void test_umeyama_and_align() {
    // Known similarity transform: rotate by +30 deg, scale by 1.5, translate (20, -10).
    const double angle = 30.0 * 3.14159265358979323846 / 180.0;
    const double s = 1.5;
    const double c = std::cos(angle) * s;
    const double sn = std::sin(angle) * s;
    const double tx = 20.0;
    const double ty = -10.0;

    lf::Points5 src{};
    for (std::size_t i = 0; i < 5; ++i) {
        const double x = lf::kArcFaceDst112[i][0];
        const double y = lf::kArcFaceDst112[i][1];
        // Map dst -> src via inverse, or test src -> dst directly.
        src[i] = {x, y};
    }
    lf::Points5 dst{};
    for (std::size_t i = 0; i < 5; ++i) {
        dst[i] = {
            c * src[i][0] - sn * src[i][1] + tx,
            sn * src[i][0] + c * src[i][1] + ty,
        };
    }
    const auto m = lf::estimate_umeyama_similarity(src, dst);
    assert(std::abs(m[0] - c) < 1e-4);
    assert(std::abs(m[1] - (-sn)) < 1e-4);
    assert(std::abs(m[2] - tx) < 1e-4);
    assert(std::abs(m[3] - sn) < 1e-4);
    assert(std::abs(m[4] - c) < 1e-4);
    assert(std::abs(m[5] - ty) < 1e-4);

    // Aligning landmarks already on kArcFaceDst112 must preserve pixels cleanly.
    auto img = std::vector<std::uint8_t>(112U * 112U * 3U, 0);
    for (int y = 20; y < 90; ++y) {
        for (int x = 20; x < 90; ++x) {
            const auto idx = (static_cast<std::size_t>(y) * 112U + static_cast<std::size_t>(x)) * 3U;
            img[idx + 0] = 100;
            img[idx + 1] = 150;
            img[idx + 2] = 200;
        }
    }
    lf::Landmarks lms{};
    for (std::size_t i = 0; i < 5; ++i) {
        lms[i] = {static_cast<float>(lf::kArcFaceDst112[i][0]),
                  static_cast<float>(lf::kArcFaceDst112[i][1])};
    }
    const auto aligned = lf::align_face_112(img, 112, 112, lms);
    assert(aligned.size() == 112U * 112U * 3U);
    const auto centre = (56U * 112U + 56U) * 3U;
    assert(aligned[centre + 0] == 100 && aligned[centre + 1] == 150 && aligned[centre + 2] == 200);

    std::vector<float> rec_in(3U * 112U * 112U);
    lf::pack_recognizer_input(aligned, rec_in);
    const auto plane = 112U * 112U;
    const auto expected_r = (100.0F / 255.0F - 0.5F) / 0.5F;
    assert(std::abs(rec_in[56U * 112U + 56U] - expected_r) < 1e-5F);
    assert(std::abs(rec_in[plane + 56U * 112U + 56U] - ((150.0F / 255.0F - 0.5F) / 0.5F)) < 1e-5F);
    assert(std::abs(rec_in[2U * plane + 56U * 112U + 56U] - ((200.0F / 255.0F - 0.5F) / 0.5F)) < 1e-5F);
}

void test_dual_scale_fas_crops() {
    auto img = std::vector<std::uint8_t>(400U * 400U * 3U, 128);
    const auto face = lf::Detection{.x1 = 150.0F, .y1 = 150.0F, .x2 = 250.0F, .y2 = 250.0F, .score = 0.9F};
    const auto crops = lf::crop_dual_scale_fas(img, 400, 400, face);
    assert(crops.texture.size() == 128U * 128U * 3U);
    assert(crops.context.size() == 128U * 128U * 3U);
    // Centre pixel of both crops is inside the 400x400 image (128).
    const auto mid = (64U * 128U + 64U) * 3U;
    assert(crops.texture[mid] == 128 && crops.context[mid] == 128);
    std::vector<float> fas_in(3U * 128U * 128U);
    lf::pack_fas_input(crops.texture, fas_in);
    assert(std::abs(fas_in[64U * 128U + 64U] - (128.0F / 255.0F)) < 1e-5F);
}

}  // namespace

int main() {
    test_letterbox_and_unletterbox();
    test_decode_and_nms();
    test_umeyama_and_align();
    test_dual_scale_fas_crops();
    std::cout << "PASS: LightFace preprocessing unit tests\n";
    return 0;
}
