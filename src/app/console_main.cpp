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
    std::cout << "Default threshold: " << snapshot->recognition_threshold << '\n';
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
    return 0;
}

