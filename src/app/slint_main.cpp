#include "app/app_controller.h"
#include "app/core_bridge.h"

#include "app_window.h"

#include <format>
#include <string>

namespace {

std::string camera_summary(const su::app::AppSnapshot& snapshot) {
    if (snapshot.cameras.empty()) {
        return "No cameras detected";
    }

    std::string text = std::format("{} camera(s)", snapshot.cameras.size());
    for (const auto& camera : snapshot.cameras) {
        text += std::format(" / [{}] {}", camera.index, camera.name);
    }
    return text;
}

slint::SharedString demo_auth_text(su::app::AppController& controller) {
    const auto demo_auth = controller.evaluate_demo_auth("demo");
    if (!demo_auth) {
        return slint::SharedString("Unavailable");
    }
    return slint::SharedString(*demo_auth ? "Accepted" : "Rejected");
}

}  // namespace

int main() {
    su::app::AppController controller;
    const auto snapshot = controller.load_initial_snapshot();
    if (!snapshot) {
        std::cerr << "su_app failed to start: " << snapshot.error() << '\n';
        return 1;
    }

    auto window = su::app::ui::AppWindow::create();
    window->set_title_text(slint::SharedString(snapshot->title));
    window->set_core_version(slint::SharedString(std::format("Rust core v{}", su::app::core_version_major())));
    window->set_threshold_text(slint::SharedString(std::format("{:.2f}", snapshot->recognition_threshold)));
    window->set_camera_text(slint::SharedString(camera_summary(*snapshot)));
    window->set_auth_text(demo_auth_text(controller));
    window->on_demo_auth_requested([window, &controller] {
        window->set_auth_text(demo_auth_text(controller));
    });
    window->run();
    return 0;
}
