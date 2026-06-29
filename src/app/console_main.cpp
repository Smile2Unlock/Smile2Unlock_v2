#include "app/app_controller.h"

#include <iostream>

int main() {
    su::app::AppController controller;
    const auto snapshot = controller.load_initial_snapshot();
    if (!snapshot) {
        std::cerr << "su_app failed to start: " << snapshot.error() << '\n';
        return 1;
    }

    std::cout << snapshot->title << '\n';
    std::cout << "Slint enabled: " << (snapshot->slint_enabled ? "yes" : "no") << '\n';
    std::cout << "Config path: " << snapshot->config_path << '\n';
    std::cout << "Profile store: " << snapshot->profile_store_path << '\n';
    std::cout << "Threshold: " << snapshot->config.recognition_threshold << '\n';
    std::cout << "Selected camera: " << snapshot->config.selected_camera << '\n';
    std::cout << "Preview FPS: " << snapshot->config.preview_fps << '\n';
    std::cout << "Liveness: " << (snapshot->config.liveness_detection ? "yes" : "no") << '\n';
    std::cout << "SeetaFace: " << (snapshot->seetaface_available ? "available" : "unavailable") << '\n';
    std::cout << "Detected cameras: " << snapshot->cameras.size() << '\n';
    for (const auto& camera : snapshot->cameras) {
        std::cout << "  [" << camera.index << "] " << camera.name << '\n';
    }

    const auto demo_auth = controller.evaluate_demo_auth("demo");
    if (!demo_auth) {
        std::cerr << "demo auth failed: " << demo_auth.error() << '\n';
        return 1;
    }
    std::cout << "Demo auth accepted: " << (*demo_auth ? "yes" : "no") << '\n';

    // Read-only: list whatever profiles already exist without enrolling a
    // demo profile into the user's real store as a startup side effect.
    const auto profiles = controller.list_face_profiles();
    if (!profiles) {
        std::cerr << "failed to list face profiles: " << profiles.error() << '\n';
        return 1;
    }
    std::cout << "Face profiles:\n" << *profiles << '\n';
    return 0;
}
