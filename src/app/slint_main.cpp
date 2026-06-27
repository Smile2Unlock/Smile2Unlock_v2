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

su::app::FaceDemoSnapshot run_face_demo_or_empty(su::app::AppController& controller) {
    const auto face_demo = controller.run_face_demo(
        "Demo Face",
        "face:demo:front",
        "face:demo:front");
    if (!face_demo) {
        return {};
    }
    return *face_demo;
}

slint::SharedString profiles_or_error(su::app::AppController& controller) {
    const auto profiles = controller.list_face_profiles();
    if (!profiles) {
        return slint::SharedString(profiles.error());
    }
    return slint::SharedString(*profiles);
}

slint::SharedString enroll_or_error(
    su::app::AppController& controller,
    const slint::SharedString& label,
    const slint::SharedString& sample_seed) {
    const auto profiles = controller.enroll_face_profile_from_sample(
        std::string(label),
        std::string(sample_seed));
    if (!profiles) {
        return slint::SharedString(profiles.error());
    }
    return slint::SharedString(*profiles);
}

su::app::FaceDemoSnapshot authenticate_or_empty(
    su::app::AppController& controller,
    const slint::SharedString& sample_seed) {
    const auto face_demo = controller.authenticate_face_sample_from_seed(std::string(sample_seed));
    if (!face_demo) {
        return {};
    }
    return *face_demo;
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
    window->set_threshold_text(slint::SharedString(std::format("{:.2f}", snapshot->config.recognition_threshold)));
    window->set_preview_fps_text(slint::SharedString(std::format("{}", snapshot->config.preview_fps)));
    window->set_liveness_text(slint::SharedString(snapshot->config.liveness_detection ? "Enabled" : "Disabled"));
    window->set_config_path_text(slint::SharedString(snapshot->config_path));
    window->set_profile_store_path_text(slint::SharedString(snapshot->profile_store_path));
    window->set_profile_text(slint::SharedString(snapshot->profiles_json));
    window->set_camera_text(slint::SharedString(camera_summary(*snapshot)));
    window->set_auth_text(demo_auth_text(controller));
    const auto face_demo = run_face_demo_or_empty(controller);
    window->set_face_auth_report_text(slint::SharedString(face_demo.auth_report_json));
    window->on_demo_auth_requested([window, &controller] {
        window->set_auth_text(demo_auth_text(controller));
        const auto face_demo = run_face_demo_or_empty(controller);
        window->set_profile_text(slint::SharedString(face_demo.profiles_json));
        window->set_face_auth_report_text(slint::SharedString(face_demo.auth_report_json));
    });
    window->on_enroll_requested([window, &controller](slint::SharedString label, slint::SharedString sample_seed) {
        window->set_profile_text(enroll_or_error(controller, label, sample_seed));
    });
    window->on_face_auth_requested([window, &controller](slint::SharedString sample_seed) {
        const auto face_demo = authenticate_or_empty(controller, sample_seed);
        window->set_profile_text(slint::SharedString(face_demo.profiles_json));
        window->set_face_auth_report_text(slint::SharedString(face_demo.auth_report_json));
        window->set_auth_text(slint::SharedString(face_demo.decision.accepted ? "Accepted" : "Rejected"));
    });
    window->on_refresh_profiles_requested([window, &controller] {
        window->set_profile_text(profiles_or_error(controller));
    });
    window->run();
    return 0;
}
