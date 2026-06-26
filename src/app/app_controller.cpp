#include "app/app_controller.h"

#include "app/core_bridge.h"

#include <format>

namespace su::app {

std::expected<AppSnapshot, std::string> AppController::load_initial_snapshot() {
    const auto threshold = default_threshold();
    if (!threshold) {
        return std::unexpected("failed to load default threshold from Rust core");
    }

    return AppSnapshot{
        .title = std::format("Smile2Unlock core v{}", core_version_major()),
        .cameras = recognizer_.enumerate_cameras(),
        .recognition_threshold = *threshold,
        .slint_enabled = SU_HAS_SLINT != 0,
    };
}

std::expected<bool, std::string> AppController::evaluate_demo_auth(std::string_view username) {
    const auto threshold = default_threshold();
    if (!threshold) {
        return std::unexpected("failed to load default threshold from Rust core");
    }

    const auto decision = evaluate_auth(username, 0.72F, *threshold, true);
    if (!decision) {
        return std::unexpected("Rust core rejected the demo auth request");
    }

    return decision->accepted;
}

}  // namespace su::app

