#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "recognizer/exposure/exposure_control.h"

using namespace su::recognizer::detail;

namespace {

// Compile-time checks: the decision logic is constexpr end to end.
static_assert(metering_region(640, 480, std::nullopt) == PixelRegion{192, 120, 256, 240});
static_assert(metering_region(640, 480, PixelRegion{100, 100, 200, 200})
    == PixelRegion{130, 130, 140, 140});
static_assert(next_exposure_step(ExposureLoopState{.level = 0, .observations_since_change = 9},
    LumaStats{.mean = 12.0, .clipped_fraction = 0.0, .samples = 10},
    ExposureKnob{.kind = ExposureKnobKind::ev_bias, .min = -6, .max = 6, .step = 1, .initial = 0})
    .apply_level == 1);
static_assert(next_exposure_step(ExposureLoopState{.level = 0, .observations_since_change = 9},
    LumaStats{.mean = 120.0, .clipped_fraction = 0.0, .samples = 10},
    ExposureKnob{.kind = ExposureKnobKind::ev_bias, .min = -6, .max = 6, .step = 1, .initial = 0})
    .apply_level == std::nullopt);
static_assert(to_string(ExposureKnobKind::manual_exposure) == "manual_exposure");

std::vector<std::byte> solid_image(
    int width, int height, int channels, std::uint8_t value) {
    return std::vector<std::byte>(
        static_cast<std::size_t>(width) * height * channels, std::byte{value});
}

ExposureKnob additive_knob() {
    return ExposureKnob{
        .kind = ExposureKnobKind::ev_bias, .min = -6, .max = 6, .step = 1, .initial = 0};
}

ExposureKnob manual_knob() {
    return ExposureKnob{
        .kind = ExposureKnobKind::manual_exposure, .min = 1, .max = 333, .step = 1,
        .initial = 100};
}

// State that has already waited long enough to act.
ExposureLoopState ready(int level) {
    return ExposureLoopState{.level = level, .observations_since_change = 100};
}

LumaStats stats(double mean, double clipped = 0.0, std::size_t samples = 100) {
    return LumaStats{.mean = mean, .clipped_fraction = clipped, .samples = samples};
}

void test_metering_region() {
    const auto centre = metering_region(640, 480, std::nullopt);
    assert(centre.width == 256 && centre.height == 240);
    assert(centre.x == 192 && centre.y == 120);

    const auto face = metering_region(640, 480, PixelRegion{100, 100, 200, 200});
    assert((face == PixelRegion{130, 130, 140, 140}));

    // A face partly outside the frame is clamped to it.
    const auto clamped = metering_region(640, 480, PixelRegion{-100, -100, 400, 400});
    assert(clamped.x >= 0 && clamped.y >= 0);
    assert(clamped.x + clamped.width <= 640 && clamped.y + clamped.height <= 480);
    assert(clamped.width > 0 && clamped.height > 0);

    // Face fully outside / empty falls back to the centre window.
    assert(metering_region(640, 480, PixelRegion{900, 900, 50, 50}) == centre);
    assert(metering_region(640, 480, PixelRegion{10, 10, 0, 0}) == centre);

    assert(metering_region(0, 480, std::nullopt) == PixelRegion{});
}

void test_measure_luma() {
    const auto rgb = solid_image(64, 48, 3, 100);
    const auto full = PixelRegion{0, 0, 64, 48};
    auto result = measure_luma(rgb, 64, 48, 3, full);
    assert(result.samples > 0);
    assert(result.mean > 98.0 && result.mean < 101.0);
    assert(result.clipped_fraction == 0.0);

    const auto rgba = solid_image(64, 48, 4, 255);
    result = measure_luma(rgba, 64, 48, 4, full);
    assert(result.mean > 250.0);
    assert(result.clipped_fraction == 1.0);

    const auto gray = solid_image(64, 48, 1, 40);
    result = measure_luma(gray, 64, 48, 1, full);
    assert(result.mean == 40.0);

    // Left half bright, right half dark: a region on the left clips, the
    // right does not, and the whole frame is half clipped.
    auto split = solid_image(64, 48, 1, 0);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 32; ++x) {
            split[static_cast<std::size_t>(y) * 64 + x] = std::byte{255};
        }
    }
    assert(measure_luma(split, 64, 48, 1, PixelRegion{0, 0, 32, 48}).clipped_fraction == 1.0);
    assert(measure_luma(split, 64, 48, 1, PixelRegion{32, 0, 32, 48}).clipped_fraction == 0.0);
    const auto half = measure_luma(split, 64, 48, 1, full).clipped_fraction;
    assert(half > 0.4 && half < 0.6);

    // Invalid geometry or short buffers yield no samples.
    assert(measure_luma(rgb, 0, 48, 3, full).samples == 0);
    assert(measure_luma(rgb, 64, 48, 2, full).samples == 0);
    assert(measure_luma(rgb, 64, 48, 3, PixelRegion{0, 0, 0, 10}).samples == 0);
    assert(measure_luma(rgb, 64, 48, 3, PixelRegion{60, 0, 10, 10}).samples == 0);
    assert(measure_luma(std::span(rgb).first(rgb.size() - 1), 64, 48, 3, full).samples == 0);

    // Large regions are subsampled.
    const auto big = solid_image(1920, 1080, 3, 90);
    result = measure_luma(big, 1920, 1080, 3, PixelRegion{0, 0, 1920, 1080});
    assert(result.samples > 0 && result.samples <= 4096);
    assert(result.mean > 88.0 && result.mean < 91.0);
}

void test_step_directions() {
    const auto knob = additive_knob();
    auto step = next_exposure_step(ready(0), stats(12.0), knob);
    assert(step.apply_level == 1 && step.next.level == 1);
    assert(step.next.observations_since_change == 0);

    step = next_exposure_step(ready(0), stats(200.0), knob);
    assert(step.apply_level == -1 && step.next.level == -1);

    // Clipping lowers even when the mean sits inside the band.
    step = next_exposure_step(ready(0), stats(120.0, 0.2), knob);
    assert(step.apply_level == -1);

    // Dark mean with heavy clipping must not raise.
    step = next_exposure_step(ready(0), stats(60.0, 0.2), knob);
    assert(step.apply_level == -1);

    // In-band hold touches nothing and keeps counting.
    step = next_exposure_step(ready(0), stats(120.0), knob);
    assert(!step.apply_level && step.next.level == 0);
    assert(step.next.observations_since_change == 101);
}

void test_step_settle_and_samples() {
    const auto knob = additive_knob();
    const auto tuning = ExposureTuning{};
    auto state = ExposureLoopState{.level = 0, .observations_since_change = 0};
    for (int i = 1; i < tuning.settle_observations; ++i) {
        const auto step = next_exposure_step(state, stats(12.0), knob);
        assert(!step.apply_level);
        assert(step.next.observations_since_change == i);
        state = step.next;
    }
    const auto step = next_exposure_step(state, stats(12.0), knob);
    assert(step.apply_level == 1);

    // No samples: no change, but the observation still counts.
    const auto empty = next_exposure_step(ready(0), stats(0.0, 0.0, 0), knob);
    assert(!empty.apply_level);
    assert(empty.next.observations_since_change == 101);
}

void test_step_clamping() {
    const auto knob = additive_knob();
    auto step = next_exposure_step(ready(6), stats(12.0), knob);
    assert(!step.apply_level && step.next.level == 6);
    step = next_exposure_step(ready(-6), stats(250.0), knob);
    assert(!step.apply_level && step.next.level == -6);

    // A level outside the range is clamped back and applied.
    step = next_exposure_step(ready(0), stats(12.0),
        ExposureKnob{.kind = ExposureKnobKind::backlight_compensation, .min = 0, .max = 2,
                     .step = 4, .initial = 0});
    assert(step.apply_level == 2);
}

void test_step_manual_exposure() {
    const auto knob = manual_knob();
    // Very dark: ratio capped at 2x.
    auto step = next_exposure_step(ready(100), stats(10.0), knob);
    assert(step.apply_level == 200);
    // Mildly dark: ratio ~1.04 still moves at least one step.
    step = next_exposure_step(ready(100), stats(85.0, 0.0), knob);
    assert(step.apply_level && *step.apply_level > 100);
    // Bright: halves at most.
    step = next_exposure_step(ready(100), stats(250.0), knob);
    assert(step.apply_level == 50);
    // Slightly too bright still moves down by at least one step.
    step = next_exposure_step(ready(100), stats(151.0), knob);
    assert(step.apply_level && *step.apply_level < 100);
    // Clipped with in-band mean moves down at least one step.
    step = next_exposure_step(ready(100), stats(120.0, 0.3), knob);
    assert(step.apply_level && *step.apply_level < 100);
    // Never above max.
    step = next_exposure_step(ready(300), stats(10.0), knob);
    assert(step.apply_level == 333);
    step = next_exposure_step(ready(333), stats(10.0), knob);
    assert(!step.apply_level);
    // Rounded to a multiple of step.
    auto coarse = knob;
    coarse.step = 10;
    step = next_exposure_step(ready(100), stats(10.0), coarse);
    assert(step.apply_level && *step.apply_level % 10 == 0);
    // Cannot go below min.
    step = next_exposure_step(ready(1), stats(250.0), knob);
    assert(!step.apply_level);
}

}  // namespace

int main() {
    test_metering_region();
    test_measure_luma();
    test_step_directions();
    test_step_settle_and_samples();
    test_step_clamping();
    test_step_manual_exposure();
    return 0;
}
