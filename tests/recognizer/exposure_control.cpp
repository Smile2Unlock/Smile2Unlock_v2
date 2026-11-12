#ifdef NDEBUG
#undef NDEBUG
#endif

#include <array>
#include <cassert>
#include <algorithm>
#include <initializer_list>
#include <ranges>
#include <type_traits>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "recognizer/exposure/exposure_control.h"

using namespace su::recognizer::detail;

namespace {

std::vector<std::byte> solid_image(
    int width, int height, int channels, std::uint8_t value);

constexpr ExposureKnob kEvKnob{
    .kind = ExposureKnobKind::ev_bias, .min = -6, .max = 6, .step = 1, .initial = 0};
constexpr ExposureKnob kBacklightKnob{
    .kind = ExposureKnobKind::backlight_compensation, .min = 0, .max = 2, .step = 1,
    .initial = 0};
constexpr ExposureKnob kManualKnob{
    .kind = ExposureKnobKind::manual_exposure, .min = 1, .max = 333, .step = 1, .initial = 100};

constexpr ExposureKnobChain chain_of(const std::initializer_list<ExposureKnob> knobs) {
    auto chain = ExposureKnobChain{};
    for (const auto& knob : knobs) {
        chain.push_back(knob);
    }
    return chain;
}

// State that has already waited long enough to act.
constexpr ExposureLoopState settled(ExposureLoopState state) {
    state.observations_since_change = 100;
    return state;
}

constexpr LumaStats dark{.mean = 12.0, .clipped_fraction = 0.0, .samples = 10};
constexpr LumaStats bright{.mean = 250.0, .clipped_fraction = 0.0, .samples = 10};
constexpr LumaStats in_band{.mean = 120.0, .clipped_fraction = 0.0, .samples = 10};

// Compile-time checks: the decision logic is constexpr end to end.
static_assert(metering_region(640, 480, std::nullopt) == PixelRegion{192, 120, 256, 240});
static_assert(metering_region(640, 480, PixelRegion{100, 100, 200, 200})
    == PixelRegion{130, 130, 140, 140});
static_assert(to_string(ExposureKnobKind::manual_exposure) == "manual_exposure");
static_assert(measure_luma(std::array{std::byte{200}, std::byte{0}, std::byte{0}}, 1, 1, 3,
                  PixelRegion{0, 0, 1, 1}).mean
    != measure_luma(std::array{std::byte{200}, std::byte{0}, std::byte{0}}, 1, 1, 3,
        PixelRegion{0, 0, 1, 1}, PixelOrder::bgr).mean);

static_assert(chain_of({kEvKnob, kBacklightKnob}).without(ExposureKnobKind::ev_bias).size() == 1);

// Dark face, saturated backlight: escalate to manual exposure.
constexpr auto kSaturated = [] {
    auto state = make_exposure_state(chain_of({kBacklightKnob, kManualKnob}));
    state.levels[std::to_underlying(ExposureKnobKind::backlight_compensation)] = 2;
    state.active = ExposureKnobKind::backlight_compensation;
    return next_exposure_step(settled(state), dark, chain_of({kBacklightKnob, kManualKnob}));
}();
static_assert(kSaturated.change
    == ExposureChange{.kind = ExposureKnobKind::manual_exposure,
                      .action = ExposureAction::set, .level = 200});
static_assert(kSaturated.next.active == ExposureKnobKind::manual_exposure);
// Nothing happens in band or with nothing to drive.
static_assert(!next_exposure_step(settled(make_exposure_state(chain_of({kEvKnob}))), in_band,
    chain_of({kEvKnob})).change);
static_assert(!next_exposure_step(settled({}), dark, chain_of({})).change);

std::vector<std::byte> solid_image(
    int width, int height, int channels, std::uint8_t value) {
    return std::vector<std::byte>(
        static_cast<std::size_t>(width) * height * channels, std::byte{value});
}

// Single-knob chain helpers for the per-knob behaviour tests.
ExposureLoopState ready(const ExposureKnob& knob, int level) {
    auto state = make_exposure_state(chain_of({knob}));
    state.levels[std::to_underlying(knob.kind)] = level;
    return settled(state);
}

ExposureStep step_for(const ExposureLoopState& state, const LumaStats& observed,
    const ExposureKnob& knob) {
    return next_exposure_step(state, observed, chain_of({knob}));
}

std::optional<int> applied(const ExposureStep& step) {
    return step.change.transform([](const ExposureChange& change) { return change.level; });
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
    const auto knob = kEvKnob;
    auto step = step_for(ready(knob, 0), stats(12.0), knob);
    assert(applied(step) == 1 && step.next.levels[std::to_underlying(knob.kind)] == 1);
    assert(step.next.observations_since_change == 0);

    step = step_for(ready(knob, 0), stats(200.0), knob);
    assert(applied(step) == -1 && step.next.levels[std::to_underlying(knob.kind)] == -1);

    // Clipping lowers even when the mean sits inside the band.
    step = step_for(ready(knob, 0), stats(120.0, 0.2), knob);
    assert(applied(step) == -1);

    // Dark mean with heavy clipping must not raise.
    step = step_for(ready(knob, 0), stats(60.0, 0.2), knob);
    assert(applied(step) == -1);

    // In-band hold touches nothing and keeps counting.
    step = step_for(ready(knob, 0), stats(120.0), knob);
    assert(!step.change && step.next.levels[std::to_underlying(knob.kind)] == 0);
    assert(step.next.observations_since_change == 101);
}

void test_step_settle_and_samples() {
    const auto knob = kEvKnob;
    const auto tuning = ExposureTuning{};
    auto state = make_exposure_state(chain_of({knob}));
    for (int i = 1; i < tuning.settle_observations; ++i) {
        const auto step = step_for(state, stats(12.0), knob);
        assert(!step.change);
        assert(step.next.observations_since_change == i);
        state = step.next;
    }
    const auto step = step_for(state, stats(12.0), knob);
    assert(applied(step) == 1);

    // No samples: no change, but the observation still counts.
    const auto empty = step_for(ready(knob, 0), stats(0.0, 0.0, 0), knob);
    assert(!empty.change);
    assert(empty.next.observations_since_change == 101);
}

void test_step_clamping() {
    const auto knob = kEvKnob;
    auto step = step_for(ready(knob, 6), stats(12.0), knob);
    assert(!step.change && step.next.levels[std::to_underlying(knob.kind)] == 6);
    step = step_for(ready(knob, -6), stats(250.0), knob);
    assert(!step.change && step.next.levels[std::to_underlying(knob.kind)] == -6);

    // A level outside the range is clamped back and applied.
    const auto coarse = ExposureKnob{.kind = ExposureKnobKind::backlight_compensation,
        .min = 0, .max = 2, .step = 4, .initial = 0};
    step = step_for(ready(coarse, 0), stats(12.0), coarse);
    assert(applied(step) == 2);
}

void test_step_manual_exposure() {
    const auto knob = kManualKnob;
    // Very dark: ratio capped at 2x.
    auto step = step_for(ready(knob, 100), stats(10.0), knob);
    assert(applied(step) == 200);
    // Mildly dark: ratio ~1.04 still moves at least one step.
    step = step_for(ready(knob, 100), stats(85.0, 0.0), knob);
    assert(applied(step) && *applied(step) > 100);
    // Bright: halves at most.
    step = step_for(ready(knob, 100), stats(250.0), knob);
    assert(applied(step) == 50);
    // Slightly too bright still moves down by at least one step.
    step = step_for(ready(knob, 100), stats(151.0), knob);
    assert(applied(step) && *applied(step) < 100);
    // Clipped with in-band mean moves down at least one step.
    step = step_for(ready(knob, 100), stats(120.0, 0.3), knob);
    assert(applied(step) && *applied(step) < 100);
    // Never above max.
    step = step_for(ready(knob, 300), stats(10.0), knob);
    assert(applied(step) == 333);
    step = step_for(ready(knob, 333), stats(10.0), knob);
    assert(!step.change);
    // Rounded to a multiple of step.
    auto coarse = knob;
    coarse.step = 10;
    step = step_for(ready(coarse, 100), stats(10.0), coarse);
    assert(applied(step) && *applied(step) % 10 == 0);
    // Cannot go below min.
    step = step_for(ready(knob, 1), stats(250.0), knob);
    assert(!step.change);
}

// Drives the chain like the service does: apply the change, keep the state,
// and let the settle window pass before the next observation.
struct Driven {
    ExposureLoopState state;
    std::vector<ExposureChange> changes;
};

Driven drive(Driven driven, const ExposureKnobChain& chain, const LumaStats& observed,
    const int observations) {
    for (int i = 0; i < observations; ++i) {
        const auto step = next_exposure_step(
            settled(driven.state), observed, chain);
        driven.state = step.next;
        if (step.change) {
            driven.changes.push_back(*step.change);
        }
    }
    return driven;
}

constexpr auto level_index(const ExposureKnobKind kind) { return std::to_underlying(kind); }

void test_escalation_when_saturated() {
    const auto chain = chain_of({kBacklightKnob, kManualKnob});
    const auto driven = drive({make_exposure_state(chain), {}}, chain, dark, 8);
    // Backlight 0 -> 1 -> 2, then manual exposure 100 -> 200 -> 333 (capped), then idle.
    assert(driven.changes.size() == 4);
    assert((driven.changes[0] == ExposureChange{ExposureKnobKind::backlight_compensation,
        ExposureAction::set, 1}));
    assert(driven.changes[1].level == 2);
    assert((driven.changes[2] == ExposureChange{ExposureKnobKind::manual_exposure,
        ExposureAction::set, 200}));
    assert(driven.changes[3].level == 333);
    assert(driven.state.active == ExposureKnobKind::manual_exposure);
}

void test_deescalation_order_and_restore() {
    const auto chain = chain_of({kBacklightKnob, kManualKnob});
    const auto up = drive({make_exposure_state(chain), {}}, chain, dark, 8);
    const auto down = drive({up.state, {}}, chain, bright, 12);
    // Manual 333 -> 167 -> 84 (<= initial 100) is a restore, not a set; then backlight 2 -> 1 -> 0.
    assert(down.changes.size() == 4);
    assert(down.changes[0].level == 167 && down.changes[0].action == ExposureAction::set);
    assert((down.changes[1] == ExposureChange{ExposureKnobKind::manual_exposure,
        ExposureAction::restore, 100}));
    assert(down.changes[2].kind == ExposureKnobKind::backlight_compensation
        && down.changes[2].level == 1);
    assert(down.changes[3].level == 0);
    assert(down.state.levels[level_index(ExposureKnobKind::manual_exposure)] == 100);
    assert(down.state.active == ExposureKnobKind::backlight_compensation);
}

void test_three_knob_chain_unwinds_in_reverse() {
    const auto chain = chain_of({kEvKnob, kBacklightKnob, kManualKnob});
    const auto up = drive({make_exposure_state(chain), {}}, chain, dark, 40);
    assert(up.state.active == ExposureKnobKind::manual_exposure);
    const auto down = drive({up.state, {}}, chain, bright, 60);
    const auto restores = down.changes | std::views::filter([](const ExposureChange& change) {
        return change.action == ExposureAction::restore;
    }) | std::views::transform(&ExposureChange::kind) | std::ranges::to<std::vector>();
    assert((restores == std::vector{ExposureKnobKind::manual_exposure,
                                    ExposureKnobKind::backlight_compensation}));
    // Everything is back at the first knob, which may go below its initial.
    assert(down.state.levels[level_index(ExposureKnobKind::backlight_compensation)] == 0);
}

void test_disabled_knob_is_skipped() {
    // Backlight failed and was dropped: ev_bias saturates straight into manual.
    const auto chain = chain_of({kEvKnob, kManualKnob});
    auto state = make_exposure_state(chain);
    state.levels[level_index(ExposureKnobKind::ev_bias)] = kEvKnob.max;
    auto step = next_exposure_step(settled(state), dark, chain);
    assert(step.change && step.change->kind == ExposureKnobKind::manual_exposure);

    // The active knob itself was dropped: carry on with what remains.
    auto stale = make_exposure_state(chain_of({kBacklightKnob, kManualKnob}));
    stale.active = ExposureKnobKind::backlight_compensation;
    step = next_exposure_step(settled(stale), dark, chain_of({kManualKnob}));
    assert(step.change && step.change->kind == ExposureKnobKind::manual_exposure);
}

void test_single_knob_and_empty_chains() {
    for (const auto& knob : {kEvKnob, kBacklightKnob, kManualKnob}) {
        const auto chain = chain_of({knob});
        const auto up = drive({make_exposure_state(chain), {}}, chain, dark, 60);
        assert(up.state.levels[level_index(knob.kind)] == knob.max);
        const auto down = drive({up.state, {}}, chain, bright, 60);
        // A lone knob is never "left": it is lowered in place, never restored.
        assert(std::ranges::none_of(down.changes, [](const ExposureChange& change) {
            return change.action == ExposureAction::restore;
        }));
        assert(down.state.levels[level_index(knob.kind)] == knob.min);
    }
    const auto empty = drive({make_exposure_state({}), {}}, {}, dark, 20);
    assert(empty.changes.empty());
}

void test_in_band_touches_nothing() {
    const auto chain = chain_of({kEvKnob, kBacklightKnob, kManualKnob});
    const auto driven = drive({make_exposure_state(chain), {}}, chain, in_band, 50);
    assert(driven.changes.empty());
    assert(!driven.state.active);
}

void test_pixel_order() {
    // Pure red pixel: RGB order weighs it 77/256, BGR order reads it as blue (29/256).
    const auto pixel = std::array{std::byte{200}, std::byte{0}, std::byte{0}};
    const auto one = PixelRegion{0, 0, 1, 1};
    const auto as_rgb = measure_luma(pixel, 1, 1, 3, one);
    const auto as_bgr = measure_luma(pixel, 1, 1, 3, one, PixelOrder::bgr);
    assert(as_rgb.mean == 60.0);   // (77 * 200) >> 8
    assert(as_bgr.mean == 22.0);   // (29 * 200) >> 8
    assert(measure_luma(pixel, 1, 1, 3, one, PixelOrder::rgb).mean == as_rgb.mean);
    // Grey and single-channel data do not depend on the order.
    const auto grey = solid_image(8, 8, 3, 100);
    const auto all = PixelRegion{0, 0, 8, 8};
    assert(measure_luma(grey, 8, 8, 3, all, PixelOrder::bgr).mean
        == measure_luma(grey, 8, 8, 3, all).mean);
}

}  // namespace

int main() {
    test_metering_region();
    test_measure_luma();
    test_step_directions();
    test_step_settle_and_samples();
    test_step_clamping();
    test_step_manual_exposure();
    test_escalation_when_saturated();
    test_deescalation_order_and_restore();
    test_three_knob_chain_unwinds_in_reverse();
    test_disabled_knob_is_skipped();
    test_single_knob_and_empty_chains();
    test_in_band_touches_nothing();
    test_pixel_order();
    return 0;
}
