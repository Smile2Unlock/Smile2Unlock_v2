#pragma once
// Backlight-aware exposure control (issue #137).
//
// When a bright window sits behind the user, the camera's own auto exposure
// meters the whole frame and leaves the face nearly black. The recognizer
// closes a small loop around the camera instead: meter the face (or the frame
// centre before a face is found) and nudge one camera control until the face
// sits in a usable brightness band — in both directions, so a face that gets
// too bright or clips is pulled back down. The background (window) is allowed
// to blow out; only the face matters for recognition.
//
// Plain header (no module, no windows.h) so it is shared by the camera module,
// the Media Foundation TU and the unit tests.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace su::recognizer::detail {

// Rectangle in frame pixel coordinates.
struct PixelRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const PixelRegion&) const = default;
};

// Camera control the closed loop drives, in order of preference. ev_bias and
// backlight_compensation keep the camera's own auto exposure running (it
// still follows lighting changes); manual_exposure turns it off, so the loop
// must keep tracking the scene itself.
enum class ExposureKnobKind { ev_bias, backlight_compensation, manual_exposure };

struct ExposureKnob {
    ExposureKnobKind kind = ExposureKnobKind::backlight_compensation;
    int min = 0;
    int max = 0;
    int step = 1;
    // Level the loop starts from. For ev_bias / backlight this is the value
    // read at open (restored on release).
    int initial = 0;
};

// Probed once when the camera opens.
struct ExposureCapabilities {
    // Camera auto exposure accepts a metering region of interest.
    bool metering_region = false;
    std::optional<ExposureKnob> knob;
};

// Luma statistics of the metered region.
struct LumaStats {
    double mean = 0.0;               // 0..255
    double clipped_fraction = 0.0;   // share of samples >= clip level
    std::size_t samples = 0;
};

struct ExposureTuning {
    double target_low = 90.0;
    double target_high = 150.0;
    std::uint8_t clip_level = 250;
    double max_clipped_fraction = 0.05;
    // Observations to wait after a change so the camera can settle before
    // the next decision (prevents oscillation).
    int settle_observations = 4;
};

struct ExposureLoopState {
    int level = 0;
    int observations_since_change = 0;
};

struct ExposureStep {
    ExposureLoopState next;
    // Set only when the camera control must change.
    std::optional<int> apply_level;
};

}  // namespace su::recognizer::detail
