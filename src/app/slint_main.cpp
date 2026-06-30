#include "app/app_controller.h"
#include "app/core_bridge.h"
#include "app/preview_controller.h"

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

std::string profile_rows_text(const std::vector<su::app::FaceProfileSummary>& profiles) {
    if (profiles.empty()) {
        return "No enrolled face profiles";
    }

    std::string text;
    for (const auto& profile : profiles) {
        text += std::format(
            "{} / {} / created_at={}\n",
            profile.id,
            profile.label,
            profile.created_at_unix);
    }
    return text;
}

su::app::FaceDemoSnapshot run_face_demo_or_empty(su::app::AppController& controller) {
    const auto face_demo = controller.run_face_demo(
        "Demo Face",
        "mock:face:demo:front",
        "mock:face:demo:front");
    if (!face_demo) {
        return {};
    }
    return *face_demo;
}

slint::SharedString profiles_or_error(su::app::AppController& controller) {
    const auto profiles = controller.list_face_profile_rows();
    if (!profiles) {
        return slint::SharedString(profiles.error());
    }
    return slint::SharedString(profile_rows_text(*profiles));
}

slint::SharedString enroll_or_error(
    su::app::AppController& controller,
    const slint::SharedString& label,
    const slint::SharedString& face_sample_source) {
    const auto profiles = controller.enroll_face_profile_from_sample(
        std::string(label),
        std::string(face_sample_source));
    if (!profiles) {
        return slint::SharedString(profiles.error());
    }
    const auto rows = controller.list_face_profile_rows();
    if (!rows) {
        return slint::SharedString(rows.error());
    }
    return slint::SharedString(profile_rows_text(*rows));
}

slint::SharedString enroll_current_frame_or_error(
    su::app::AppController& controller,
    const slint::SharedString& label) {
    const auto profiles = controller.enroll_face_profile_from_current_frame(std::string(label));
    if (!profiles) {
        return slint::SharedString(profiles.error());
    }
    const auto rows = controller.list_face_profile_rows();
    if (!rows) {
        return slint::SharedString(rows.error());
    }
    return slint::SharedString(profile_rows_text(*rows));
}

slint::SharedString delete_or_error(
    su::app::AppController& controller,
    const slint::SharedString& profile_id) {
    const auto deleted = controller.delete_face_profile_by_id(std::string(profile_id));
    if (!deleted) {
        return slint::SharedString(deleted.error());
    }
    const auto profiles = controller.list_face_profile_rows();
    if (!profiles) {
        return slint::SharedString(profiles.error());
    }
    auto text = profile_rows_text(*profiles);
    if (!*deleted) {
        text = "No profile deleted\n" + text;
    }
    return slint::SharedString(text);
}

su::app::FaceDemoSnapshot authenticate_or_empty(
    su::app::AppController& controller,
    const slint::SharedString& face_sample_source) {
    const auto face_demo = controller.authenticate_face_sample_from_source(std::string(face_sample_source));
    if (!face_demo) {
        return {};
    }
    return *face_demo;
}

su::app::FaceDemoSnapshot authenticate_current_frame_or_empty(
    su::app::AppController& controller,
    slint::SharedString& error_text) {
    const auto face_demo = controller.authenticate_current_frame();
    if (!face_demo) {
        error_text = slint::SharedString(face_demo.error());
        return {};
    }
    return *face_demo;
}

}  // namespace

int main() {
    su::app::AppController controller;
    su::app::PreviewController preview;
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
    window->set_profile_text(slint::SharedString(profile_rows_text(snapshot->profiles)));
    window->set_camera_text(slint::SharedString(camera_summary(*snapshot)));
    window->set_seetaface_text(slint::SharedString(snapshot->seetaface_available ? "Available" : "Unavailable"));
    window->set_auth_text(demo_auth_text(controller));
    // Do not run the face demo at startup: it would enroll a "Demo Face"
    // profile into the user's real profile store as a side effect. Show
    // placeholders instead; the Run Demo Auth button triggers it on demand.
    window->set_auth_score_text(slint::SharedString("-"));
    window->set_auth_best_profile_text(slint::SharedString("-"));
    window->set_auth_reason_text(slint::SharedString("Not checked"));
    window->set_debug_json_text(slint::SharedString(
        "Sample sources: mock:<id>, image:<path> (real SeetaFace when available), embedding:<comma-separated floats>"));
    window->on_demo_auth_requested([window, &controller] {
        window->set_auth_text(demo_auth_text(controller));
        const auto face_demo = run_face_demo_or_empty(controller);
        window->set_profile_text(slint::SharedString(profile_rows_text(face_demo.profiles)));
        window->set_auth_score_text(slint::SharedString(std::format("{:.4f}", face_demo.report.score)));
        window->set_auth_best_profile_text(slint::SharedString(face_demo.report.best_profile_label));
        window->set_auth_reason_text(slint::SharedString(face_demo.report.reason));
        window->set_debug_json_text(slint::SharedString(face_demo.auth_report_json));
    });
    window->on_enroll_requested([window, &controller](slint::SharedString label, slint::SharedString face_sample_source) {
        window->set_profile_text(enroll_or_error(controller, label, face_sample_source));
    });
    window->on_enroll_current_frame_requested([window, &controller](slint::SharedString label) {
        window->set_profile_text(enroll_current_frame_or_error(controller, label));
    });
    window->on_face_auth_requested([window, &controller](slint::SharedString face_sample_source) {
        const auto face_demo = authenticate_or_empty(controller, face_sample_source);
        window->set_profile_text(slint::SharedString(profile_rows_text(face_demo.profiles)));
        window->set_auth_score_text(slint::SharedString(std::format("{:.4f}", face_demo.report.score)));
        window->set_auth_best_profile_text(slint::SharedString(face_demo.report.best_profile_label));
        window->set_auth_reason_text(slint::SharedString(face_demo.report.reason));
        window->set_debug_json_text(slint::SharedString(face_demo.auth_report_json));
        window->set_auth_text(slint::SharedString(face_demo.decision.accepted ? "Accepted" : "Rejected"));
    });
    window->on_current_frame_auth_requested([window, &controller] {
        auto error_text = slint::SharedString();
        const auto face_demo = authenticate_current_frame_or_empty(controller, error_text);
        if (!error_text.empty()) {
            window->set_auth_text(slint::SharedString("Unavailable"));
            window->set_auth_reason_text(error_text);
            return;
        }
        window->set_profile_text(slint::SharedString(profile_rows_text(face_demo.profiles)));
        window->set_auth_score_text(slint::SharedString(std::format("{:.4f}", face_demo.report.score)));
        window->set_auth_best_profile_text(slint::SharedString(face_demo.report.best_profile_label));
        window->set_auth_reason_text(slint::SharedString(face_demo.report.reason));
        window->set_debug_json_text(slint::SharedString(face_demo.auth_report_json));
        window->set_auth_text(slint::SharedString(face_demo.decision.accepted ? "Accepted" : "Rejected"));
    });
    window->on_delete_profile_requested([window, &controller](slint::SharedString profile_id) {
        window->set_profile_text(delete_or_error(controller, profile_id));
    });
    window->on_refresh_profiles_requested([window, &controller] {
        window->set_profile_text(profiles_or_error(controller));
    });

    // Preview: the capture thread pushes frames onto the event loop; this
    // callback runs on the UI thread and updates the image plus the face-box
    // overlay. The box is scaled from the 640x480 capture frame to the 320x240
    // preview area (0.5x).
    auto push_preview_frame = [window](slint::Image image, su::app::PreviewOverlay overlay) {
        window->set_preview_image(std::move(image));
        window->set_preview_status_text(slint::SharedString(overlay.status_text));
        if (overlay.face_box) {
            window->set_face_box_x(overlay.face_box->x * 0.5F);
            window->set_face_box_y(overlay.face_box->y * 0.5F);
            window->set_face_box_w(overlay.face_box->width * 0.5F);
            window->set_face_box_h(overlay.face_box->height * 0.5F);
            window->set_face_box_visible(true);
        } else {
            window->set_face_box_visible(false);
        }
    };

    window->on_start_preview_requested([window, &controller, &preview, push_preview_frame] {
        if (preview.is_running()) {
            return;
        }
        const auto snapshot = controller.load_config_snapshot();
        const auto fps = snapshot ? snapshot->preview_fps : 15;
        const auto camera = snapshot ? snapshot->selected_camera : 0;
        const auto liveness_enabled = snapshot ? snapshot->liveness_detection : true;
        preview.start(controller.recognizer(), camera, static_cast<int>(fps),
                      liveness_enabled,
                      push_preview_frame);
        window->set_preview_status_text(slint::SharedString("starting"));
    });
    window->on_stop_preview_requested([&preview] {
        preview.stop();
    });

    window->run();
    preview.stop();
    return 0;
}
