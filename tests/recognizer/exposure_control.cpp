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

// A time far past any settle or flip-flop window, for decisions that are not
// about timing.
constexpr ExposureTime kLater{3600 * 1000};

constexpr ExposureStep decide(const ExposureLoopState& state, const LumaStats& observed,
    const ExposureKnobChain& chain, const ExposureTime now = kLater,
    const ExposureTuning& tuning = {}) {
    return next_exposure_step(state, observed, chain, now, tuning);
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
    return decide(settled(state), dark, chain_of({kBacklightKnob, kManualKnob}));
}();
static_assert(kSaturated.change
    == ExposureChange{.kind = ExposureKnobKind::manual_exposure,
                      .action = ExposureAction::set, .level = 100});  // engage: initial, unscaled
static_assert(kSaturated.next.manual_engaged);
static_assert(kSaturated.next.active == ExposureKnobKind::manual_exposure);
// Nothing happens in band or with nothing to drive.
static_assert(!decide(settled(make_exposure_state(chain_of({kEvKnob}))), in_band,
    chain_of({kEvKnob})).change);
static_assert(!decide(settled({}), dark, chain_of({})).change);
// Wall time gates a decision even when enough observations have passed.
static_assert(!next_exposure_step([] {
    auto state = settled({});
    state.last_change_at = ExposureTime{1000};
    return state;
}(), dark, chain_of({kEvKnob}), ExposureTime{1200}).change);
// Reversing onto the level just left is held.
static_assert(!decide(settled(decide(settled(make_exposure_state(chain_of({kEvKnob}))), dark,
    chain_of({kEvKnob}), ExposureTime{1000}).next), bright, chain_of({kEvKnob}),
    ExposureTime{3000}).change);
static_assert(!chain_of({ExposureKnob{.kind = ExposureKnobKind::ev_bias, .min = 2, .max = 1}}).size());

std::vector<std::byte> solid_image(
    int width, int height, int channels, std::uint8_t value) {
    return std::vector<std::byte>(
        static_cast<std::size_t>(width) * height * channels, std::byte{value});
}

// Single-knob chain helpers for the per-knob behaviour tests.
ExposureLoopState ready(const ExposureKnob& knob, int level) {
    auto state = make_exposure_state(chain_of({knob}));
    state.levels[std::to_underlying(knob.kind)] = level;
    state.manual_engaged = knob.kind == ExposureKnobKind::manual_exposure;
    return settled(state);
}

ExposureStep step_for(const ExposureLoopState& state, const LumaStats& observed,
    const ExposureKnob& knob) {
    return decide(state, observed, chain_of({knob}));
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
    // Waits settle_observations observations, then the next one decides.
    for (int i = 1; i <= tuning.settle_observations; ++i) {
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
    step = step_for(ready(knob, 300), stats(250.0), knob);
    assert(applied(step) == 150);
    // Slightly too bright still moves down by at least one step.
    step = step_for(ready(knob, 300), stats(151.0), knob);
    assert(applied(step) && *applied(step) < 300);
    // Clipped with in-band mean moves down at least one step.
    step = step_for(ready(knob, 300), stats(120.0, 0.3), knob);
    assert(applied(step) && *applied(step) < 300);
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
    // Falling to the initial level or below hands control back to auto.
    step = step_for(ready(knob, 100), stats(250.0), knob);
    assert(step.change && step.change->action == ExposureAction::restore
        && step.change->level == knob.initial);
}

// Drives the chain like the service does: apply the change, keep the state,
// and let the settle window pass before the next observation.
struct Driven {
    ExposureLoopState state;
    std::vector<ExposureChange> changes;
    ExposureTime now{0};
};

constexpr ExposureTime kObservationInterval{2000};
constexpr ExposureTime kPastFlipFlopMemory{60000};

// The next phase of a scenario, once the anti flip-flop memory has lapsed.
Driven later(Driven driven) {
    driven.changes.clear();
    driven.now += kPastFlipFlopMemory;
    return driven;
}

Driven drive(Driven driven, const ExposureKnobChain& chain, const LumaStats& observed,
    const int observations) {
    for (int i = 0; i < observations; ++i) {
        driven.now += kObservationInterval;
        const auto step = decide(settled(driven.state), observed, chain, driven.now);
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
    // Backlight 0 -> 1 -> 2, then manual exposure engaged at its initial 100,
    // then 200 -> 333 (capped), then idle.
    assert(driven.changes.size() == 5);
    assert((driven.changes[0] == ExposureChange{ExposureKnobKind::backlight_compensation,
        ExposureAction::set, 1}));
    assert(driven.changes[1].level == 2);
    assert((driven.changes[2] == ExposureChange{ExposureKnobKind::manual_exposure,
        ExposureAction::set, 100}));
    assert(driven.changes[3].level == 200);
    assert(driven.changes[4].level == 333);
    assert(driven.state.active == ExposureKnobKind::manual_exposure);
}

void test_deescalation_order_and_restore() {
    const auto chain = chain_of({kBacklightKnob, kManualKnob});
    const auto up = drive({make_exposure_state(chain), {}}, chain, dark, 8);
    const auto down = drive(later(up), chain, bright, 12);
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
    const auto down = drive(later(up), chain, bright, 60);
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
    auto step = decide(settled(state), dark, chain);
    assert(step.change && step.change->kind == ExposureKnobKind::manual_exposure);

    // The active knob itself was dropped: carry on with what remains.
    auto stale = make_exposure_state(chain_of({kBacklightKnob, kManualKnob}));
    stale.active = ExposureKnobKind::backlight_compensation;
    step = decide(settled(stale), dark, chain_of({kManualKnob}));
    assert(step.change && step.change->kind == ExposureKnobKind::manual_exposure);
}

void test_single_knob_and_empty_chains() {
    for (const auto& knob : {kEvKnob, kBacklightKnob, kManualKnob}) {
        const auto chain = chain_of({knob});
        const auto up = drive({make_exposure_state(chain), {}}, chain, dark, 60);
        assert(up.state.levels[level_index(knob.kind)] == knob.max);
        const auto down = drive(later(up), chain, bright, 60);
        const auto restores = std::ranges::count_if(down.changes, [](const ExposureChange& change) {
            return change.action == ExposureAction::restore;
        });
        if (knob.kind == ExposureKnobKind::manual_exposure) {
            // Manual exposure has nothing below auto: it hands control back.
            assert(restores == 1);
            assert(down.state.levels[level_index(knob.kind)] == knob.initial);
            assert(!down.state.manual_engaged);
        } else {
            // A lone additive knob is lowered in place, never restored.
            assert(restores == 0);
            assert(down.state.levels[level_index(knob.kind)] == knob.min);
        }
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


// Applies one decision the way the service does: the camera applies `snap`
// of the requested level.
ExposureStep apply(const ExposureStep& step, const int applied_level) {
    auto result = step;
    if (step.change && step.change->action == ExposureAction::set) {
        result.next = with_applied_level(step.next, step.change->kind, applied_level);
    }
    return result;
}

void test_anti_flip_flop() {
    const auto chain = chain_of({kEvKnob});
    // Too dark: 0 -> 1. Too bright right after: lowering would land on 0, the
    // level just left, so the loop holds instead of hunting.
    const auto up = decide(ready(kEvKnob, 0), dark, chain, ExposureTime{1000});
    assert(applied(up) == 1);
    const auto back = decide(settled(up.next), bright, chain, ExposureTime{3000});
    assert(!back.change);
    assert(back.next.levels[level_index(kEvKnob.kind)] == 1);
    // Same direction is never held.
    assert(applied(decide(settled(up.next), dark, chain, ExposureTime{3000})) == 2);
    // The memory lapses: a scene that really changed gets corrected.
    assert(applied(decide(settled(up.next), bright, chain, ExposureTime{40000})) == 0);
}

void test_anti_flip_flop_clip_cycle_and_snapping() {
    // Backlight 1 -> 2, then clipped: lowering would land back on 1, hold.
    const auto chain = chain_of({kBacklightKnob});
    const auto up = decide(ready(kBacklightKnob, 1), dark, chain, ExposureTime{1000});
    assert(applied(up) == 2);
    const auto clipped = LumaStats{.mean = 120.0, .clipped_fraction = 0.3, .samples = 10};
    assert(!decide(settled(up.next), clipped, chain, ExposureTime{3000}).change);

    // The camera snaps 150 to 160, then 300 to 320. The level left by the
    // second move is the APPLIED 160, so a drop back to 160 is a reversal.
    const auto manual = chain_of({kManualKnob});
    const auto first = apply(decide(ready(kManualKnob, 100), stats(80.0), manual,
        ExposureTime{1000}), 160);
    assert(first.next.levels[level_index(kManualKnob.kind)] == 160);
    const auto second = apply(decide(settled(first.next), stats(10.0), manual,
        ExposureTime{3000}), 320);
    assert(second.next.last_move && second.next.last_move->from_level == 160);
    assert(!decide(settled(second.next), stats(250.0), manual, ExposureTime{5000}).change);
}

void test_wall_time_settle() {
    const auto chain = chain_of({kEvKnob});
    auto state = ready(kEvKnob, 0);
    state.last_change_at = ExposureTime{1000};
    // Enough observations, too little time.
    assert(!decide(state, dark, chain, ExposureTime{1499}).change);
    assert(applied(decide(state, dark, chain, ExposureTime{1500})) == 1);
    // Enough time, too few observations.
    state.observations_since_change = 0;
    assert(!decide(state, dark, chain, ExposureTime{9000}).change);
    // The step records when it changed.
    const auto step = decide(ready(kEvKnob, 0), dark, chain, ExposureTime{7000});
    assert(step.next.last_change_at == ExposureTime{7000});
}

void test_manual_first_write_is_unscaled() {
    const auto chain = chain_of({kManualKnob});
    const auto engage = decide(settled(make_exposure_state(chain)), stats(10.0), chain,
        ExposureTime{1000});
    assert((engage.change == ExposureChange{ExposureKnobKind::manual_exposure,
        ExposureAction::set, 100}));
    assert(engage.next.manual_engaged);
    // Proportional from then on (ratio capped at 2x).
    assert(applied(decide(settled(engage.next), stats(10.0), chain, ExposureTime{3000})) == 200);
    // Too bright while auto still rules: nothing to lower.
    assert(!decide(settled(make_exposure_state(chain)), bright, chain).change);
}

void test_single_manual_knob_restores_to_auto() {
    const auto chain = chain_of({kManualKnob});
    auto state = ready(kManualKnob, 200);
    state.manual_engaged = true;
    const auto down = decide(state, bright, chain);  // 200 * 0.5 = 100 == initial
    assert((down.change == ExposureChange{ExposureKnobKind::manual_exposure,
        ExposureAction::restore, 100}));
    assert(!down.next.manual_engaged);
    assert(down.next.levels[level_index(kManualKnob.kind)] == 100);
}

void test_applied_level_bookkeeping() {
    const auto chain = chain_of({kManualKnob});
    const auto step = decide(ready(kManualKnob, 100), stats(10.0), chain);
    assert(applied(step) == 200);
    const auto snapped = with_applied_level(step.next, ExposureKnobKind::manual_exposure, 180);
    assert(snapped.levels[level_index(kManualKnob.kind)] == 180);
    // The next step starts from 180: doubling gives 360, capped at the maximum.
    assert(applied(decide(settled(snapped), stats(10.0), chain, kLater + ExposureTime{10000})) == 333);
}

void test_chain_rejects_invalid_knobs() {
    auto chain = ExposureKnobChain{};
    assert(!chain.push_back({.kind = ExposureKnobKind::ev_bias, .min = 3, .max = 1}));
    assert(chain.empty());
    assert(chain.push_back(kEvKnob));
    assert(!chain.push_back(kEvKnob));  // one knob per kind
    assert(chain.size() == 1);
}

void test_region_math_survives_extreme_geometry() {
    constexpr auto huge = 2'000'000'000;
    const auto region = metering_region(640, 480, PixelRegion{huge, huge, huge, huge});
    assert(region.x >= 0 && region.y >= 0 && region.x + region.width <= 640
        && region.y + region.height <= 480);
    const auto wide = metering_region(640, 480, PixelRegion{-huge, -huge, huge, huge});
    assert(wide.x + wide.width <= 640 && wide.y + wide.height <= 480);
    assert(measure_luma(solid_image(2, 2, 3, 10), 2, 2, 3, PixelRegion{huge, 0, huge, 1}).samples == 0);
}

}  // namespace

// The whole-session restore (user switch off) must name every knob exactly
// once, so none is left changed in the camera.
void test_restore_order_covers_every_knob() {
    static_assert(kRestoreOrder.size() == kExposureKnobKindCount);
    static_assert(kRestoreOrder.front() == ExposureKnobKind::manual_exposure);
    auto seen = std::array<int, kExposureKnobKindCount>{};
    for (const auto kind : kRestoreOrder) {
        ++seen[std::to_underlying(kind)];
    }
    assert(std::ranges::all_of(seen, [](const int count) { return count == 1; }));
}

using RestoreResult = std::expected<void, ExposureWriteError>;

// A failed restore is remembered only when retrying can help: transient errors
// stay pending, a rejection or missing control is given up on, success clears.
void test_restore_pending_bookkeeping() {
    auto pending = RestorePending{};
    assert(!pending.any());
    pending = record_knob_restore(pending, ExposureKnobKind::ev_bias,
        std::unexpected(ExposureWriteError::transient));
    pending = record_region_restore(pending, std::unexpected(ExposureWriteError::transient));
    assert(pending.any() && pending.region);
    assert(pending.knobs[std::to_underlying(ExposureKnobKind::ev_bias)]);
    assert(!pending.knobs[std::to_underlying(ExposureKnobKind::manual_exposure)]);

    pending = record_knob_restore(pending, ExposureKnobKind::ev_bias, RestoreResult{});
    assert(!pending.knobs[std::to_underlying(ExposureKnobKind::ev_bias)] && pending.any());

    pending = record_region_restore(pending, std::unexpected(ExposureWriteError::rejected));
    assert(!pending.region && !pending.any());
    pending = record_knob_restore(pending, ExposureKnobKind::manual_exposure,
        std::unexpected(ExposureWriteError::unsupported));
    assert(!pending.any());
}

// Retries are throttled and only run while something is pending.
void test_restore_retry_due() {
    const auto none = RestorePending{};
    auto some = RestorePending{};
    some.region = true;
    constexpr auto next_due = ExposureTime{1000};
    assert(!restore_retry_due(none, ExposureTime{5000}, next_due));
    assert(!restore_retry_due(some, ExposureTime{999}, next_due));
    assert(restore_retry_due(some, ExposureTime{1000}, next_due));
}

int main() {
    test_restore_pending_bookkeeping();
    test_restore_retry_due();
    test_restore_order_covers_every_knob();
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
    test_anti_flip_flop();
    test_anti_flip_flop_clip_cycle_and_snapping();
    test_wall_time_settle();
    test_manual_first_write_is_unscaled();
    test_single_manual_knob_restores_to_auto();
    test_applied_level_bookkeeping();
    test_chain_rejects_invalid_knobs();
    test_region_math_survives_extreme_geometry();
    return 0;
}
