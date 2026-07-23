#include "app_window.h"

import std;
import su.app.controller;
import su.app.i18n;
import su.app.user;
import su.core.types;
import su.app.preview;
import su.app.session;
import su.app.theme;

namespace {

namespace ui = su::app::ui;

using ProfileModel = slint::VectorModel<ui::ProfileRow>;
using WindowHandle = slint::ComponentHandle<ui::AppWindow>;
using WeakWindowHandle = slint::ComponentWeakHandle<ui::AppWindow>;

constexpr std::string_view xdg_app_id = "smile2unlock";

std::string camera_summary(const su::app::AppSnapshot& snapshot) {
    if (snapshot.cameras.size() == 1) {
        return snapshot.cameras.front().name;
    }
    return {};
}

slint::Color slint_color(su::app::ThemeColor color) {
    return slint::Color::from_argb_uint8(color.alpha, color.red, color.green, color.blue);
}

void apply_theme(const WindowHandle& window, const su::app::AppTheme& snapshot) {
    const auto& theme = window->global<ui::UiTheme>();
    theme.set_canvas(slint_color(snapshot.canvas));
    theme.set_surface(slint_color(snapshot.surface));
    theme.set_surface_subtle(slint_color(snapshot.surface_subtle));
    theme.set_surface_selected(slint_color(snapshot.surface_selected));
    theme.set_border(slint_color(snapshot.border));
    theme.set_divider(slint_color(snapshot.divider));
    theme.set_text_primary(slint_color(snapshot.text_primary));
    theme.set_text_secondary(slint_color(snapshot.text_secondary));
    theme.set_text_tertiary(slint_color(snapshot.text_tertiary));
    theme.set_primary(slint_color(snapshot.primary));
    theme.set_primary_hover(slint_color(snapshot.primary_hover));
    theme.set_primary_pressed(slint_color(snapshot.primary_pressed));
    theme.set_on_primary(slint_color(snapshot.on_primary));
    theme.set_success_surface(slint_color(snapshot.success_surface));
    theme.set_success_text(slint_color(snapshot.success_text));
    theme.set_warning_surface(slint_color(snapshot.warning_surface));
    theme.set_warning_text(slint_color(snapshot.warning_text));
    theme.set_danger_surface(slint_color(snapshot.danger_surface));
    theme.set_danger_hover(slint_color(snapshot.danger_hover));
    theme.set_danger_text(slint_color(snapshot.danger_text));
    theme.set_disabled_surface(slint_color(snapshot.disabled_surface));
    theme.set_disabled_text(slint_color(snapshot.disabled_text));
    theme.set_preview_surface(slint_color(snapshot.preview_surface));
    theme.set_preview_overlay(slint_color(snapshot.preview_overlay));
    theme.set_preview_text(slint_color(snapshot.preview_text));
    theme.set_face_indicator(slint_color(snapshot.face_indicator));
    theme.set_dark_mode(snapshot.mode == su::app::ThemeMode::dark);
}

void log_theme_diagnostics(const std::vector<std::string>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::println(stderr, "[theme] {}", diagnostic);
    }
}

std::string username_initial(std::string_view username) {
    if (username.empty()) {
        return "U";
    }
    auto initial = static_cast<char>(std::toupper(static_cast<unsigned char>(username.front())));
    return std::string(1, initial);
}

std::string enrollment_date(std::uint64_t created_at_unix) {
    if (created_at_unix > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return "-";
    }

    using namespace std::chrono;
    const auto timestamp = sys_seconds{seconds{static_cast<std::int64_t>(created_at_unix)}};
    const auto date = year_month_day{floor<days>(timestamp)};
    if (!date.ok()) {
        return "-";
    }
    return std::format(
        "{:04}-{:02}-{:02}",
        static_cast<int>(date.year()),
        static_cast<unsigned>(date.month()),
        static_cast<unsigned>(date.day()));
}

std::vector<ui::ProfileRow> profile_rows(
    const std::vector<su::app::FaceProfileSummary>& profiles) {
    std::vector<ui::ProfileRow> rows;
    rows.reserve(profiles.size());
    for (const auto& profile : profiles) {
        rows.push_back(ui::ProfileRow{
            .id = slint::SharedString(profile.id),
            .label = slint::SharedString(profile.label),
            .enrolled_at = slint::SharedString(enrollment_date(profile.created_at_unix)),
        });
    }
    return rows;
}

void update_profiles(
    const std::shared_ptr<ProfileModel>& model,
    const std::vector<su::app::FaceProfileSummary>& profiles) {
    model->set_vector(profile_rows(profiles));
}

void set_activity(
    const WindowHandle& window,
    std::string_view title,
    std::string_view detail,
    std::string_view tone,
    bool busy = false) {
    window->set_activity_title(slint::SharedString(title));
    window->set_activity_detail(slint::SharedString(detail));
    window->set_activity_tone(slint::SharedString(tone));
    window->set_busy(busy);
}

std::size_t language_index(const WindowHandle& window) {
    return static_cast<std::size_t>(std::max(window->get_language_index(), 0));
}

std::string translated(
    const su::app::LanguageCatalog& catalog,
    const WindowHandle& window,
    std::string_view key) {
    return catalog.translate(language_index(window), key);
}

std::string translated_value(
    const su::app::LanguageCatalog& catalog,
    const WindowHandle& window,
    std::string_view key,
    std::string_view value) {
    return catalog.translate_value(language_index(window), key, value);
}

void set_preview_idle(
    const WindowHandle& window,
    const su::app::LanguageCatalog& catalog) {
    window->set_preview_status_text(slint::SharedString(translated(catalog, window, "preview.idle")));
    window->set_face_box_visible(false);
}

std::filesystem::path executable_directory(const char* argument_zero) {
    std::error_code error;
    auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        return executable.parent_path();
    }
    executable = std::filesystem::absolute(argument_zero, error);
    return error ? std::filesystem::current_path() : executable.parent_path();
}

std::filesystem::path language_directory(const std::filesystem::path& executable_dir) {
    const auto candidates = std::array{
        executable_dir / "assets" / "i18n",
        executable_dir / ".." / "share" / "smile2unlock" / "i18n",
    };
    const auto found = std::ranges::find_if(candidates, [](const auto& candidate) {
        auto error = std::error_code{};
        return std::filesystem::is_directory(candidate, error);
    });
    return found == candidates.end() ? candidates.front() : *found;
}

std::filesystem::path ui_preference_path() {
    if (const auto* config_home = std::getenv("XDG_CONFIG_HOME");
        config_home != nullptr && *config_home != '\0') {
        return std::filesystem::path(config_home) / "smile2unlock" / "ui.json";
    }
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".config" / "smile2unlock" / "ui.json";
    }
    return std::filesystem::current_path() / ".smile2unlock-ui.json";
}

std::string system_locale() {
    for (const auto* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        if (const auto* value = std::getenv(name); value != nullptr && *value != '\0') {
            return value;
        }
    }
    return "en";
}

}  // namespace

int main(int argc, char** argv) {
    slint::set_xdg_app_id(xdg_app_id);
    const auto theme_paths = su::app::default_theme_paths();
    const auto theme_commands = su::app::system_theme_command_runner();
    auto initial_theme = su::app::load_desktop_theme(theme_paths, theme_commands);
    log_theme_diagnostics(initial_theme.diagnostics);
    const auto language_path = language_directory(
        executable_directory(argc > 0 ? argv[0] : "su_app"));
    auto loaded_catalog = su::app::LanguageCatalog::load(language_path);
    if (!loaded_catalog) {
        std::cerr << "su_app failed to load language packs: " << loaded_catalog.error() << '\n';
        return 1;
    }
    const auto catalog = std::make_shared<const su::app::LanguageCatalog>(std::move(*loaded_catalog));
    const auto preference_path = ui_preference_path();
    auto loaded_preference = su::app::load_language_preference(preference_path);
    std::optional<std::string> preferred_language;
    if (loaded_preference) {
        preferred_language = std::move(*loaded_preference);
    } else {
        std::println(stderr, "[i18n] {}", loaded_preference.error());
    }
    const auto selected_language = catalog->select_language(preferred_language, system_locale());

    auto controller = std::make_shared<su::app::AppController>();
    auto preview = std::make_shared<su::app::PreviewController>();
    const auto snapshot = controller->load_initial_snapshot();
    if (!snapshot) {
        std::cerr << "su_app failed to start: " << snapshot.error() << '\n';
        return 1;
    }

    auto window = ui::AppWindow::create();
    apply_theme(window, initial_theme.snapshot.theme);
    const WeakWindowHandle weak_window(window);
    const auto theme_monitor = std::make_unique<su::app::ThemeMonitor>(
        theme_paths,
        theme_commands,
        initial_theme.snapshot,
        [weak_window](su::app::ThemeLoadResult loaded) {
            log_theme_diagnostics(loaded.diagnostics);
            slint::invoke_from_event_loop(
                [weak_window, snapshot = std::move(loaded.snapshot)] {
                    if (const auto window = weak_window.lock()) {
                        apply_theme(*window, snapshot.theme);
                    }
                });
        });
    const auto profiles = std::make_shared<ProfileModel>(profile_rows(snapshot->profiles));
    const auto session_lock_monitor = std::make_unique<su::app::SessionLockMonitor>(
        [weak_window, controller, preview, catalog] {
            slint::invoke_from_event_loop([weak_window, controller, preview, catalog] {
                const auto preview_was_running = preview->is_running();
                preview->stop();
                controller->cancel_camera_operation();
                if (preview_was_running) {
                    const auto window = weak_window.lock();
                    if (!window) {
                        return;
                    }
                    set_preview_idle(*window, *catalog);
                }
                std::println(stderr, "[session] GUI camera released for session lock");
            });
        });

    window->on_translate([catalog](slint::SharedString key, int index) {
        return slint::SharedString(catalog->translate(
            static_cast<std::size_t>(std::max(index, 0)), std::string_view(key)));
    });
    window->on_translate_value(
        [catalog](slint::SharedString key, int index, slint::SharedString value) {
            return slint::SharedString(catalog->translate_value(
                static_cast<std::size_t>(std::max(index, 0)),
                std::string_view(key),
                std::string_view(value)));
        });
    std::vector<slint::SharedString> language_names;
    for (const auto& name : catalog->language_names()) {
        language_names.emplace_back(name);
    }
    window->set_language_names(
        std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(language_names)));
    window->set_language_index(static_cast<int>(selected_language));
    window->on_language_selected([catalog, preference_path](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= catalog->size()) {
            return;
        }
        if (const auto saved = su::app::save_language_preference(
                preference_path, catalog->language_code(static_cast<std::size_t>(index)));
            !saved) {
            std::println(stderr, "[i18n] {}", saved.error());
        }
    });

    std::vector<slint::SharedString> camera_names;
    std::vector<int> camera_indices;
    camera_names.reserve(snapshot->cameras.size());
    camera_indices.reserve(snapshot->cameras.size());
    auto selected_camera = 0;
    for (const auto& camera : snapshot->cameras) {
        if (camera.index == snapshot->config.selected_camera) {
            selected_camera = static_cast<int>(camera_indices.size());
        }
        camera_names.emplace_back(std::format("{}: {}", camera.index, camera.name));
        camera_indices.push_back(camera.index);
    }

    const auto username = su::app::current_username(
        catalog->translate(selected_language, "common.current_user"));
    window->set_title_text(slint::SharedString(snapshot->title));
    window->set_username(slint::SharedString(username));
    window->set_username_initial(slint::SharedString(username_initial(username)));
    window->set_core_version(slint::SharedString(catalog->translate_value(
        selected_language,
        "diagnostics.core_version",
        std::format("{}", su::app::core_version_major()))));
    window->set_config_path_text(slint::SharedString(snapshot->config_path));
    window->set_profile_store_path_text(slint::SharedString(snapshot->profile_store_path));
    window->set_profiles(profiles);
    window->set_camera_options(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(camera_names)));
    window->set_camera_text(slint::SharedString(camera_summary(*snapshot)));
    window->set_camera_count(static_cast<int>(snapshot->cameras.size()));
    window->set_seetaface_available(snapshot->seetaface_available);
    window->set_control_socket_present(std::filesystem::exists("/run/smile2unlock/control.sock"));
    window->set_selected_camera(selected_camera);
    window->set_recognition_threshold(snapshot->config.recognition_threshold);
    window->set_liveness_enabled(snapshot->config.liveness_detection);
    window->set_liveness_threshold(snapshot->config.liveness_threshold);
    window->set_preview_fps(static_cast<int>(snapshot->config.preview_fps));
    set_preview_idle(window, *catalog);
    window->set_activity_title(slint::SharedString(catalog->translate(selected_language, "activity.not_checked")));
    window->set_settings_status(slint::SharedString(catalog->translate(selected_language, "settings.saved")));

    window->on_enroll_current_frame_requested(
        [weak_window, controller, preview, profiles, catalog](slint::SharedString requested_label) {
            if (auto window = weak_window.lock()) {
                preview->stop();
                set_preview_idle(*window, *catalog);
                set_activity(
                    *window,
                    translated(*catalog, *window, "activity.capturing_face"),
                    translated(*catalog, *window, "activity.look_camera"),
                    "warn",
                    true);
            }

            auto label = std::string(requested_label);
            if (label.empty()) {
                if (const auto window = weak_window.lock()) {
                    label = translated(*catalog, *window, "enrollment.default_label");
                }
            }
            std::thread([weak_window, controller, profiles, catalog, label = std::move(label)] {
                const auto enrolled = controller->enroll_face_profile_from_current_frame(label);
                auto rows = enrolled
                    ? controller->list_face_profile_rows()
                    : std::expected<std::vector<su::app::FaceProfileSummary>, std::string>{
                        std::unexpected(enrolled.error())};

                slint::invoke_from_event_loop(
                    [weak_window, profiles, catalog, label, rows = std::move(rows)]() mutable {
                        const auto window = weak_window.lock();
                        if (!window) {
                            return;
                        }
                        if (!rows) {
                            set_activity(
                                *window,
                                translated(*catalog, *window, "activity.enrollment_failed"),
                                rows.error(),
                                "bad");
                            return;
                        }
                        update_profiles(profiles, *rows);
                        set_activity(
                            *window,
                            translated(*catalog, *window, "activity.enrolled"),
                            translated_value(*catalog, *window, "activity.enrolled_detail", label),
                            "good");
                    });
            }).detach();
        });

    window->on_current_frame_auth_requested([weak_window, controller, preview, profiles, catalog] {
        if (auto window = weak_window.lock()) {
            preview->stop();
            set_preview_idle(*window, *catalog);
            set_activity(
                *window,
                translated(*catalog, *window, "activity.checking_identity"),
                translated(*catalog, *window, "activity.keep_centered"),
                "warn",
                true);
        }

        std::thread([weak_window, controller, profiles, catalog] {
            auto result = controller->authenticate_current_frame();
            slint::invoke_from_event_loop(
                [weak_window, profiles, catalog, result = std::move(result)]() mutable {
                    const auto window = weak_window.lock();
                    if (!window) {
                        return;
                    }
                    if (!result) {
                        set_activity(
                            *window,
                            translated(*catalog, *window, "activity.auth_unavailable"),
                            result.error(),
                            "bad");
                        return;
                    }

                    update_profiles(profiles, result->profiles);
                    const auto detail = result->report.best_profile_label.empty()
                        ? translated_value(
                            *catalog,
                            *window,
                            "activity.auth_score",
                            std::format("{:.2f}", result->report.score))
                        : translated_value(
                            *catalog,
                            *window,
                            "activity.auth_matched",
                            std::format(
                                "{} ({:.2f})",
                                result->report.best_profile_label,
                                result->report.score));
                    set_activity(
                        *window,
                        translated(
                            *catalog,
                            *window,
                            result->decision.accepted
                                ? "activity.auth_passed"
                                : "activity.auth_rejected"),
                        detail,
                        result->decision.accepted ? "good" : "bad");
                });
        }).detach();
    });

    window->on_delete_profile_requested(
        [weak_window, controller, profiles, catalog](slint::SharedString profile_id) {
            const auto deleted = controller->delete_face_profile_by_id(std::string(profile_id));
            const auto window = weak_window.lock();
            if (!window) {
                return;
            }
            if (!deleted) {
                set_activity(
                    *window,
                    translated(*catalog, *window, "activity.remove_failed"),
                    deleted.error(),
                    "bad");
                return;
            }
            if (!*deleted) {
                set_activity(
                    *window,
                    translated(*catalog, *window, "activity.sample_not_found"),
                    translated(*catalog, *window, "activity.refresh_retry"),
                    "bad");
                return;
            }

            const auto rows = controller->list_face_profile_rows();
            if (!rows) {
                set_activity(
                    *window,
                    translated(*catalog, *window, "activity.removed"),
                    translated(*catalog, *window, "activity.refresh_after_remove_failed"),
                    "warn");
                return;
            }
            update_profiles(profiles, *rows);
            set_activity(
                *window,
                translated(*catalog, *window, "activity.removed"),
                translated(*catalog, *window, "activity.removed_detail"),
                "good");
        });

    window->on_refresh_profiles_requested([weak_window, controller, profiles, catalog] {
        const auto rows = controller->list_face_profile_rows();
        const auto window = weak_window.lock();
        if (!window) {
            return;
        }
        if (!rows) {
            set_activity(
                *window,
                translated(*catalog, *window, "activity.refresh_failed"),
                rows.error(),
                "bad");
            return;
        }
        update_profiles(profiles, *rows);
        set_activity(
            *window,
            translated(*catalog, *window, "activity.refreshed"),
            translated(*catalog, *window, "activity.refreshed_detail"),
            "good");
    });

    window->on_migrate_profiles_requested([weak_window, controller, profiles, catalog] {
        if (const auto window = weak_window.lock()) {
            (*window)->set_busy(true);
        }
        std::thread([weak_window, controller, profiles, catalog] {
            const auto migrated = controller->migrate_legacy_profiles();
            auto rows = migrated
                ? controller->list_face_profile_rows()
                : std::expected<std::vector<su::app::FaceProfileSummary>, std::string>{
                    std::unexpected(migrated.error())};
            slint::invoke_from_event_loop(
                [weak_window, profiles, catalog, migrated, rows = std::move(rows)]() mutable {
                    const auto window = weak_window.lock();
                    if (!window) {
                        return;
                    }
                    (*window)->set_busy(false);
                    if (!migrated || !rows) {
                        set_activity(
                            *window,
                            translated(*catalog, *window, "activity.migration_failed"),
                            migrated ? rows.error() : migrated.error(),
                            "bad");
                        return;
                    }
                    update_profiles(profiles, *rows);
                    set_activity(
                        *window,
                        translated(
                            *catalog,
                            *window,
                            *migrated ? "activity.migration_done" : "activity.migration_none"),
                        translated(*catalog, *window, "activity.refreshed_detail"),
                        *migrated ? "good" : "neutral");
                });
        }).detach();
    });

    window->on_save_settings_requested(
        [weak_window, controller, camera_indices, catalog](
            int camera_selection,
            float recognition_threshold,
            bool liveness_enabled,
            float liveness_threshold,
            int preview_fps) {
            const auto window = weak_window.lock();
            if (!window) {
                return;
            }

            auto config = controller->load_config_snapshot();
            if (!config) {
                (*window)->set_settings_status(slint::SharedString(config.error()));
                set_activity(
                    *window,
                    translated(*catalog, *window, "activity.settings_unavailable"),
                    config.error(),
                    "bad");
                return;
            }
            if (camera_selection >= 0
                && static_cast<std::size_t>(camera_selection) < camera_indices.size()) {
                config->selected_camera = camera_indices[static_cast<std::size_t>(camera_selection)];
            }
            config->recognition_threshold = recognition_threshold;
            config->liveness_detection = liveness_enabled;
            config->liveness_threshold = liveness_threshold;
            config->preview_fps = static_cast<std::uint32_t>(std::clamp(preview_fps, 1, 60));

            const auto saved = controller->save_config_snapshot(*config);
            if (!saved) {
                (*window)->set_settings_status(slint::SharedString(saved.error()));
                set_activity(
                    *window,
                    translated(*catalog, *window, "activity.settings_not_saved"),
                    saved.error(),
                    "bad");
                return;
            }
            (*window)->set_settings_status(slint::SharedString(
                translated(*catalog, *window, "settings.saved")));
            set_activity(
                *window,
                translated(*catalog, *window, "settings.saved"),
                translated(*catalog, *window, "activity.settings_applied"),
                "good");
        });

    // Preview frames arrive on Slint's event loop. Keep only a weak window
    // handle so stopping the UI also releases the capture callback cleanly.
    auto push_preview_frame = [weak_window, catalog](
                                  slint::Image image,
                                  su::app::PreviewOverlay overlay) {
        const auto window = weak_window.lock();
        if (!window) {
            return;
        }
        (*window)->set_preview_image(std::move(image));
        const auto status = overlay.status_text == "preview.face_liveness"
            ? translated_value(
                *catalog,
                *window,
                overlay.status_text,
                std::format("{:.2f}", overlay.liveness_score))
            : translated(*catalog, *window, overlay.status_text);
        (*window)->set_preview_status_text(slint::SharedString(status));
        if (overlay.face_box && overlay.source_width > 0 && overlay.source_height > 0) {
            (*window)->set_preview_source_width(overlay.source_width);
            (*window)->set_preview_source_height(overlay.source_height);
            (*window)->set_face_box_x(static_cast<float>(overlay.face_box->x));
            (*window)->set_face_box_y(static_cast<float>(overlay.face_box->y));
            (*window)->set_face_box_w(static_cast<float>(overlay.face_box->width));
            (*window)->set_face_box_h(static_cast<float>(overlay.face_box->height));
            (*window)->set_face_box_visible(true);
        } else {
            (*window)->set_face_box_visible(false);
        }
    };

    window->on_start_preview_requested(
        [weak_window, controller, preview, push_preview_frame, catalog] {
        if (preview->is_running()) {
            return;
        }
        const auto config = controller->load_config_snapshot();
        const auto window = weak_window.lock();
        if (!window) {
            return;
        }
        if (!config) {
            (*window)->set_preview_status_text(slint::SharedString(config.error()));
            return;
        }

        (*window)->set_preview_status_text(slint::SharedString(
            translated(*catalog, *window, "preview.starting")));
        preview->start(
            controller->recognizer(),
            config->selected_camera,
            static_cast<int>(config->preview_fps),
            config->liveness_detection,
            push_preview_frame);
        if (!preview->is_running()) {
            (*window)->set_preview_status_text(slint::SharedString(
                translated(*catalog, *window, "preview.start_failed")));
        }
    });

    window->on_stop_preview_requested([weak_window, preview, catalog] {
        preview->stop();
        if (const auto window = weak_window.lock()) {
            set_preview_idle(*window, *catalog);
        }
    });

    window->run();
    preview->stop();
    return 0;
}
