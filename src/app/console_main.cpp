import std;
import su.app.controller;

int main() {
    su::app::AppController controller;
    const auto snapshot = controller.load_initial_snapshot();
    if (!snapshot) {
        std::println(stderr, "su_app failed to start: {}", snapshot.error());
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
        std::println(stderr, "demo auth failed: {}", demo_auth.error());
        return 1;
    }
    std::println("Demo auth accepted: {}", *demo_auth ? "yes" : "no");

    const auto profiles = controller.list_face_profiles();
    if (!profiles) {
        std::println(stderr, "failed to list face profiles: {}", profiles.error());
        return 1;
    }
    std::println("Face profiles:\n{}", *profiles);
    return 0;
}
