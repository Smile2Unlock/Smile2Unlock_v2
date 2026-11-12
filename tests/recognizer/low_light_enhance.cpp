#ifdef NDEBUG
#undef NDEBUG
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "recognizer/exposure/low_light_enhance.h"

namespace {

using namespace su::recognizer::detail;

LumaStats stats_with_mean(const double mean) {
    return LumaStats{.mean = mean, .clipped_fraction = 0.0, .samples = 100};
}

void assert_lut_invariants(const std::array<std::uint8_t, 256>& lut) {
    assert(lut[0] == 0);
    assert(lut[255] == 255);
    assert(std::ranges::is_sorted(lut));
}

}  // namespace

int main() {
    // No enhancement when there is nothing to measure or the centre is bright.
    assert(!low_light_lut(LumaStats{}));
    assert(!low_light_lut(stats_with_mean(kLowLightMeanThreshold)));
    assert(!low_light_lut(stats_with_mean(180.0)));

    // A dark centre gets a monotonic LUT that lifts the mean toward the target.
    for (const auto mean : {1.0, 12.0, 30.0, 60.0}) {
        const auto lut = low_light_lut(stats_with_mean(mean));
        assert(lut);
        assert_lut_invariants(*lut);
        const auto lifted = static_cast<double>((*lut)[static_cast<std::size_t>(mean)]);
        assert(lifted > mean);
        assert(lifted <= kLowLightTargetMean + 1.0);
    }
    const auto moderate = low_light_lut(stats_with_mean(40.0));
    assert(moderate);
    assert(std::abs(static_cast<double>((*moderate)[40]) - kLowLightTargetMean) < 3.0);

    // Pitch black (and a near-zero mean) must not blow up the gamma: the clamp
    // keeps the LUT well-formed and identical between extremes.
    const auto black = low_light_lut(stats_with_mean(0.0));
    const auto near_black = low_light_lut(stats_with_mean(0.001));
    assert(black && near_black);
    assert_lut_invariants(*black);
    assert(*black == *near_black);
    const auto floor_gamma = std::pow(128.0 / 255.0, kLowLightMinGamma) * 255.0;
    assert(std::abs(static_cast<double>((*black)[128]) - floor_gamma) <= 1.0);

    // apply_lut returns a new buffer and leaves the input alone.
    auto inverting = std::array<std::uint8_t, 256>{};
    for (auto i = 0U; i < inverting.size(); ++i) {
        inverting[i] = static_cast<std::uint8_t>(255U - i);
    }
    const std::vector<unsigned char> input{0, 10, 128, 255};
    const auto input_copy = input;
    const auto mapped = apply_lut(input, inverting);
    assert(input == input_copy);
    assert((mapped == std::vector<unsigned char>{255, 245, 127, 0}));
    assert(apply_lut({}, inverting).empty());

    // The span overload writes into caller storage, matches the returning
    // overload, and leaves the input alone.
    auto scratch = std::vector<unsigned char>(input.size(), 0xAB);
    apply_lut(input, inverting, scratch);
    assert(input == input_copy);
    assert(scratch == mapped);
    // Reusing a scratch buffer from a previous, different frame is exact.
    const std::vector<unsigned char> next_frame{1, 2, 3, 4};
    apply_lut(next_frame, inverting, scratch);
    assert((scratch == std::vector<unsigned char>{254, 253, 252, 251}));

    // Face-brightness floor: an enhanced-only detection is trusted only when
    // the original face area carries real signal.
    assert(!enhanced_face_is_usable(LumaStats{}));
    assert(!enhanced_face_is_usable(stats_with_mean(0.0)));
    assert(!enhanced_face_is_usable(stats_with_mean(kLowLightFaceMeanFloor - 0.01)));
    assert(enhanced_face_is_usable(stats_with_mean(kLowLightFaceMeanFloor)));
    assert(enhanced_face_is_usable(stats_with_mean(200.0)));
    return 0;
}
