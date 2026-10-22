#include "common/log/logger.h"
#include <cstdio>

import std;
import su.app.controller;

int main() {
    su::app::AppController controller;
    const auto snapshot = controller.load_initial_snapshot();
    if (!snapshot) {
        su::log::process_logger().error("app.start_failed", {su::log::field("cause", snapshot.error())});
        return 1;
    }

    std::println("{}", snapshot->title);
    std::println("Slint enabled: {}", snapshot->slint_enabled ? "yes" : "no");
    std::println("Config path: {}", snapshot->config_path);
    std::println("Profile store: {}", snapshot->profile_store_path);
    std::println("Threshold: {:.2f}", snapshot->config.recognition_threshold);
    std::println("Selected camera: {}", snapshot->config.selected_camera);
    std::println("Preview FPS: {}", snapshot->config.preview_fps);
    std::println("Liveness: {}", snapshot->config.liveness_detection ? "yes" : "no");
    std::println("SeetaFace: {}", snapshot->seetaface_available ? "available" : "unavailable");
    std::println("Detected cameras: {}", snapshot->cameras.size());
    for (const auto& camera : snapshot->cameras) {
        std::println("  [{}] {}", camera.index, camera.name);
    }

    const auto demo_auth = controller.evaluate_demo_auth("demo");
    if (!demo_auth) {
        su::log::process_logger().warn("app.demo_auth_failed", {su::log::field("cause", demo_auth.error())});
        return 1;
    }
    std::println("Demo auth accepted: {}", *demo_auth ? "yes" : "no");

    const auto profiles = controller.list_face_profile_rows();
    if (!profiles) {
        su::log::process_logger().warn("storage.list_failed", {su::log::field("cause", profiles.error())});
        return 1;
    }
    std::println("Face profiles: {}", profiles->size());
    for (const auto& profile : *profiles) {
        std::println("  {} / {} / created_at={}", profile.id, profile.label, profile.created_at_unix);
    }
    return 0;
}
