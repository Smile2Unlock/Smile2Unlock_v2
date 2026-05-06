module;

#include <core/dsl.h>
#include <components/components.h>
#include <GLFW/glfw3.h>

export module su.app.ui.layout;

import std;
import su.app.i18n;
import su.app.state;
import su.app.pages;
import su.app.ui.theme;
import su.app.services.backend;

namespace {

using su::app::app_state;
using su::app::update_state;
using su::app::PageId;
using su::app::AppState;
using su::app::with_active_page;
using su::app::with_sidebar_collapsed;
using su::app::i18n::text;
using su::app::i18n::app_i18n;
using components::theme::withAlpha;
using components::theme::withOpacity;

// ---- UI state (file-scope) ----
float pageScroll[4] = {};
bool dialogOpen = false;
std::string feedbackText = "Ready";
bool toastVisible = false;
int settingsSegment = 0;

// ---- helpers ----
constexpr float kSidebarWidth = 272.0f;
constexpr float kNavHeight = 50.0f;
constexpr float kNavGap = 14.0f;

core::Color borderColor(float a = 1.0f) {
    return withOpacity(components::theme::DarkThemeColors().border, a);
}

core::Color shadowColor(float alpha = 0.28f) {
    return core::Color{0.0f, 0.0f, 0.0f, alpha};
}

core::Color accent() {
    return components::theme::DarkThemeColors().primary;
}

static auto themeColors = components::theme::DarkThemeColors;
static auto pageVisuals = [](const auto& tokens) { return components::theme::pageVisuals(tokens); };

core::Color textPrimary() {
    return core::Color{0.94f, 0.97f, 1.0f, 1.0f};
}

core::Color textMuted() {
    return core::Color{0.56f, 0.62f, 0.72f, 1.0f};
}

core::Color surface() {
    return components::theme::DarkThemeColors().surface;
}

core::Color surfaceSoft() {
    return components::theme::DarkThemeColors().surfaceHover;
}

core::Color surfaceActive() {
    return components::theme::DarkThemeColors().surfaceActive;
}

core::Color appBg() {
    return core::Color{0.07f, 0.08f, 0.10f, 1.0f};
}

core::Color mixTheme(core::Color from, core::Color to, float amount) {
    return core::mixColor(from, to, amount);
}

core::Color buttonHover(const core::Color& base) {
    return mixTheme(base, core::Color{1.0f, 1.0f, 1.0f, base.a}, 0.16f);
}

core::Color buttonPressed(const core::Color& base) {
    return mixTheme(base, core::Color{0.0f, 0.0f, 0.0f, base.a}, 0.34f);
}

core::Transition pageTransition() {
    return core::Transition::make(0.28f, core::Ease::OutCubic);
}

// ---- nav item ----
void navItem(core::dsl::Ui& ui, const std::string& id, const std::string& label, unsigned int icon, PageId page) {
    const bool active = su::app::app_state().active_page == page;
    const core::Color activeAccent = accent();
    const core::Color normal = active ? activeAccent : surface();
    const core::Color hover = active ? buttonHover(activeAccent) : surfaceSoft();
    const core::Color pressed = active ? buttonPressed(activeAccent) : surfaceActive();

    components::button(ui, id)
        .size(212.0f, kNavHeight)
        .icon(icon)
        .iconSize(16.0f)
        .fontSize(17.0f)
        .text(label)
        .colors(normal, hover, pressed)
        .textColor(textPrimary())
        .iconColor(textPrimary())
        .radius(12.0f)
        .border(1.0f, active ? withAlpha(activeAccent, 0.58f) : borderColor(0.60f))
        .shadow(12.0f, 0.0f, 4.0f, shadowColor(0.18f))
        .transition(pageTransition())
        .onClick([page] {
            update_state([page](const AppState& s) { return with_active_page(s, page); });
        })
        .build();
}

// ---- sidebar ----
void composeSidebar(core::dsl::Ui& ui, float height) {
    const core::Color sidebarBg = mixTheme(appBg(), core::Color{0.0f, 0.0f, 0.0f, 1.0f}, 0.24f);

    ui.stack("sidebar")
        .size(kSidebarWidth, height)
        .content([&] {
            ui.rect("sidebar.bg")
                .size(kSidebarWidth, height)
                .color(sidebarBg)
                .build();

            ui.column("sidebar.content")
                .size(kSidebarWidth, std::max(0.0f, height - 42.0f))
                .margin(0.0f, 30.0f, 0.0f, 0.0f)
                .gap(kNavGap)
                .alignItems(core::Align::CENTER)
                .content([&] {
                    // brand
                    ui.text("brand.icon")
                        .size(212.0f, 34.0f)
                        .icon(0xF5FD)
                        .fontSize(27.0f)
                        .lineHeight(32.0f)
                        .color(accent())
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .build();

                    ui.text("brand.title")
                        .size(212.0f, 36.0f)
                        .text("Smile2Unlock")
                        .fontSize(26.0f)
                        .lineHeight(32.0f)
                        .color(textPrimary())
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .build();

                    navItem(ui, "nav.dashboard", "Dashboard",   0xF0DB, PageId::Dashboard);
                    navItem(ui, "nav.enrollment", "Enrollment", 0xF234, PageId::Enrollment);
                    navItem(ui, "nav.recognition", "Recognition", 0xF030, PageId::Recognition);
                    navItem(ui, "nav.settings",   "Settings",   0xF013, PageId::Settings);
                });

            // theme toggle at bottom
            ui.stack("sidebar.theme")
                .x(30.0f)
                .y(std::max(0.0f, height - 82.0f))
                .size(212.0f, 50.0f)
                .content([&] {
                    components::button(ui, "nav.theme")
                        .size(212.0f, 50.0f)
                        .icon(0xF185)
                        .iconSize(16.0f)
                        .fontSize(17.0f)
                        .text("Theme")
                        .colors(surface(), surfaceSoft(), surfaceActive())
                        .textColor(textPrimary())
                        .radius(12.0f)
                        .border(1.0f, borderColor(0.80f))
                        .shadow(12.0f, 0.0f, 4.0f, shadowColor(0.18f))
                        .transition(pageTransition())
                        .build();
                });
        });
}

// ============================================================
// Pages
// ============================================================

// ---- stat card (dashboard) ----
void statCard(core::dsl::Ui& ui, const std::string& id,
              const std::string& title, const std::string& value,
              unsigned int icon, const core::Color& accentColor,
              float width, float height) {
    ui.stack(id)
        .size(width, height)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, height)
                .color(surface())
                .radius(18.0f)
                .border(1.0f, borderColor())
                .build();

            ui.text(id + ".icon")
                .x(18.0f)
                .y(18.0f)
                .size(36.0f, 36.0f)
                .icon(icon)
                .fontSize(24.0f)
                .lineHeight(34.0f)
                .color(accentColor)
                .build();

            ui.text(id + ".value")
                .x(width - 18.0f)
                .y(18.0f)
                .size(std::max(0.0f, width - 48.0f), 36.0f)
                .text(value)
                .fontSize(28.0f)
                .lineHeight(34.0f)
                .color(textPrimary())
                .horizontalAlign(core::HorizontalAlign::Right)
                .build();

            ui.text(id + ".title")
                .x(18.0f)
                .y(height - 34.0f)
                .size(std::max(0.0f, width - 36.0f), 24.0f)
                .text(title)
                .fontSize(16.0f)
                .lineHeight(22.0f)
                .color(textMuted())
                .build();
        });
}

void composeDashboardPage(core::dsl::Ui& ui, float width, float height) {
    (void)height;
    const auto& state = su::app::app_state();
    const float cardWidth = std::max(160.0f, std::min(240.0f, (width - 36.0f) / 3.0f));
    const float cardHeight = 128.0f;

    ui.row("dash.stats")
        .size(cardWidth * 3.0f + 36.0f, cardHeight)
        .gap(18.0f)
        .content([&] {
            statCard(ui, "dash.users", "Total Users",
                     std::to_string(state.total_users), 0xF0C0,
                     accent(), cardWidth, cardHeight);
            statCard(ui, "dash.faces", "Total Faces",
                     std::to_string(state.total_faces), 0xF2C0,
                     core::Color{0.20f, 0.75f, 0.45f, 1.0f}, cardWidth, cardHeight);
            statCard(ui, "dash.camera", "Camera",
                     state.camera_connected ? "Online" : "Offline", 0xF030,
                     state.camera_connected ? core::Color{0.30f, 0.55f, 0.95f, 1.0f}
                                            : core::Color{0.85f, 0.25f, 0.30f, 1.0f},
                     cardWidth, cardHeight);
        });

    ui.text("dash.section.recent")
        .size(width, 30.0f)
        .text("Recent Activity")
        .fontSize(24.0f)
        .lineHeight(30.0f)
        .color(textPrimary())
        .build();

    // user list
    ui.column("dash.user.list")
        .size(width, std::min(height * 0.5f, 320.0f))
        .gap(10.0f)
        .content([&] {
            for (const auto& user : state.users) {
                const std::string uid = "dash.user." + std::to_string(user.user_id);
                ui.stack(uid)
                    .size(width, 56.0f)
                    .content([&, user] {
                        ui.rect(uid + ".bg")
                            .size(width, 56.0f)
                            .color(surface())
                            .radius(14.0f)
                            .border(1.0f, borderColor())
                            .build();

                        ui.text(uid + ".name")
                            .x(18.0f)
                            .y(8.0f)
                            .size(width - 170.0f, 26.0f)
                            .text(user.username)
                            .fontSize(20.0f)
                            .lineHeight(24.0f)
                            .color(textPrimary())
                            .build();

                        ui.text(uid + ".faces")
                            .x(18.0f)
                            .y(34.0f)
                            .size(width - 170.0f, 20.0f)
                            .text(std::to_string(user.face_count) + " faces")
                            .fontSize(14.0f)
                            .lineHeight(18.0f)
                            .color(textMuted())
                            .build();

                        ui.text(uid + ".remark")
                            .x(width - 120.0f)
                            .y(16.0f)
                            .size(100.0f, 24.0f)
                            .text(user.remark)
                            .fontSize(14.0f)
                            .lineHeight(20.0f)
                            .color(textMuted())
                            .horizontalAlign(core::HorizontalAlign::Right)
                            .build();
                    });
            }
        });
}

void composeEnrollmentPage(core::dsl::Ui& ui, float width, float height) {
    (void)height;
    const auto& state = su::app::app_state();
    const float listWidth = std::max(260.0f, std::min(480.0f, width * 0.55f));
    const float sectionWidth = std::max(0.0f, width - listWidth - 20.0f);

    ui.row("enroll.row")
        .size(width, std::max(300.0f, height - 40.0f))
        .gap(20.0f)
        .content([&] {
            // user list
            ui.column("enroll.list")
                .size(listWidth, 400.0f)
                .gap(8.0f)
                .content([&] {
                    ui.text("enroll.list.title")
                        .size(listWidth, 30.0f)
                        .text("Users")
                        .fontSize(22.0f)
                        .lineHeight(28.0f)
                        .color(textPrimary())
                        .build();

                    for (const auto& user : state.users) {
                        const bool selected = state.selected_user_id == user.user_id;
                        const std::string uid = "enroll.user." + std::to_string(user.user_id);
                        const auto bg = selected ? accent() : surface();
                        const auto bgH = selected ? buttonHover(accent()) : surfaceSoft();
                        const auto bgP = selected ? buttonPressed(accent()) : surfaceActive();
                        components::button(ui, uid)
                            .size(listWidth, 48.0f)
                            .text(user.username + " (" + std::to_string(user.face_count) + ")")
                            .fontSize(16.0f)
                            .colors(bg, bgH, bgP)
                            .textColor(selected ? core::Color{1.0f, 1.0f, 1.0f, 1.0f} : textPrimary())
                            .radius(12.0f)
                            .border(1.0f, borderColor(0.50f))
                            .transition(pageTransition())
                            .onClick([id = user.user_id] {
                                update_state([id](const AppState& s) {
                                    return with_selected_user(s, s.selected_user_id == id ? -1 : id);
                                });
                            })
                            .build();
                    }
                });

            // detail panel
            ui.stack("enroll.detail")
                .size(sectionWidth, 300.0f)
                .content([&] {
                    if (state.selected_user_id > 0) {
                        ui.rect("enroll.detail.bg")
                            .size(sectionWidth, 260.0f)
                            .color(surface())
                            .radius(18.0f)
                            .border(1.0f, borderColor())
                            .build();

                        ui.text("enroll.detail.hint")
                            .x(20.0f)
                            .y(100.0f)
                            .size(sectionWidth - 40.0f, 30.0f)
                            .text("Selected user detail panel")
                            .fontSize(18.0f)
                            .lineHeight(26.0f)
                            .color(textMuted())
                            .horizontalAlign(core::HorizontalAlign::Center)
                            .build();
                    } else {
                        ui.text("enroll.detail.empty")
                            .x(0.0f)
                            .y(100.0f)
                            .size(sectionWidth, 30.0f)
                            .text("Select a user to view details")
                            .fontSize(18.0f)
                            .lineHeight(26.0f)
                            .color(textMuted())
                            .horizontalAlign(core::HorizontalAlign::Center)
                            .build();
                    }
                });
        });
}

void composeRecognitionPage(core::dsl::Ui& ui, float width, float height) {
    const auto& state = su::app::app_state();
    const float stageWidth = std::max(280.0f, std::min(width * 0.6f, 640.0f));
    const float sideWidth = std::max(0.0f, width - stageWidth - 24.0f);
    const float stageH = std::min(height, 480.0f);

    ui.row("recog.row")
        .size(width, height)
        .gap(24.0f)
        .content([&] {
            // left: camera stage
            ui.stack("recog.stage")
                .size(stageWidth, stageH)
                .content([&] {
                    ui.rect("recog.stage.bg")
                        .size(stageWidth, stageH)
                        .color(surface())
                        .radius(22.0f)
                        .border(1.0f, borderColor())
                        .build();

                    ui.text("recog.stage.placeholder")
                        .size(stageWidth, stageH - 80.0f)
                        .text(state.recognition_running ? "Camera feed active" : "Camera idle")
                        .fontSize(22.0f)
                        .lineHeight(30.0f)
                        .color(textMuted())
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .build();

                    const float btnY = stageH - 80.0f;
                    ui.stack("recog.btn.wrap")
                        .x((stageWidth - 200.0f) * 0.5f)
                        .y(btnY)
                        .size(200.0f, 52.0f)
                        .content([&] {
                            if (!state.recognition_running) {
                                components::button(ui, "recog.start")
                                    .size(200.0f, 52.0f)
                                    .icon(0xF04B)
                                    .fontSize(18.0f)
                                    .text("Start")
                                    .colors(accent(), buttonHover(accent()), buttonPressed(accent()))
                                    .textColor(core::Color{1.0f, 1.0f, 1.0f, 1.0f})
                                    .radius(14.0f)
                                    .shadow(16.0f, 0.0f, 6.0f, shadowColor(0.24f))
                                    .transition(pageTransition())
                                    .onClick([] {
                                        update_state([](const AppState& s) {
                                            return with_recognition_running(s, true);
                                        });
                                    })
                                    .build();
                            } else {
                                components::button(ui, "recog.stop")
                                    .size(200.0f, 52.0f)
                                    .icon(0xF04D)
                                    .fontSize(18.0f)
                                    .text("Stop")
                                    .colors(core::Color{0.85f, 0.25f, 0.30f, 1.0f},
                                            buttonHover(core::Color{0.85f, 0.25f, 0.30f, 1.0f}),
                                            buttonPressed(core::Color{0.85f, 0.25f, 0.30f, 1.0f}))
                                    .textColor(core::Color{1.0f, 1.0f, 1.0f, 1.0f})
                                    .radius(14.0f)
                                    .shadow(16.0f, 0.0f, 6.0f, shadowColor(0.24f))
                                    .transition(pageTransition())
                                    .onClick([] {
                                        update_state([](const AppState& s) {
                                            return with_recognition_running(s, false);
                                        });
                                    })
                                    .build();
                            }
                        })
                        .build();
                })
                .build();

            // right: info panel
            ui.column("recog.info")
                .size(sideWidth, stageH)
                .gap(14.0f)
                .content([&] {
                    ui.rect("recog.info.bg")
                        .size(sideWidth, stageH)
                        .color(surface())
                        .radius(18.0f)
                        .border(1.0f, borderColor())
                        .build();

                    ui.text("recog.info.status.title")
                        .x(16.0f)
                        .y(16.0f)
                        .size(sideWidth - 32.0f, 24.0f)
                        .text("Status")
                        .fontSize(18.0f)
                        .lineHeight(24.0f)
                        .color(textPrimary())
                        .build();

                    ui.text("recog.info.status.val")
                        .x(16.0f)
                        .y(44.0f)
                        .size(sideWidth - 32.0f, 22.0f)
                        .text(state.recognition_status)
                        .fontSize(15.0f)
                        .lineHeight(20.0f)
                        .color(textMuted())
                        .build();

                    if (state.last_result.success) {
                        ui.text("recog.info.result.title")
                            .x(16.0f)
                            .y(80.0f)
                            .size(sideWidth - 32.0f, 24.0f)
                            .text("Last Match")
                            .fontSize(18.0f)
                            .lineHeight(24.0f)
                            .color(textPrimary())
                            .build();

                        ui.text("recog.info.result.name")
                            .x(16.0f)
                            .y(108.0f)
                            .size(sideWidth - 32.0f, 22.0f)
                            .text(state.last_result.username)
                            .fontSize(16.0f)
                            .lineHeight(20.0f)
                            .color(accent())
                            .build();

                        char simBuf[32];
                        std::snprintf(simBuf, sizeof(simBuf), "%.1f%%", state.last_result.similarity * 100.0f);
                        ui.text("recog.info.result.sim")
                            .x(16.0f)
                            .y(132.0f)
                            .size(sideWidth - 32.0f, 22.0f)
                            .text(simBuf)
                            .fontSize(15.0f)
                            .lineHeight(20.0f)
                            .color(textMuted())
                            .build();
                    }
                })
                .build();
        });
}

void composeSettingsPage(core::dsl::Ui& ui, float width, float height) {
    (void)height;
    const auto& state = su::app::app_state();
    const float fieldWidth = std::max(260.0f, std::min(width, 600.0f));

    ui.column("settings.list")
        .size(fieldWidth, std::min(height, 440.0f))
        .gap(16.0f)
        .content([&] {
            ui.text("settings.general.title")
                .size(fieldWidth, 30.0f)
                .text("General")
                .fontSize(24.0f)
                .lineHeight(30.0f)
                .color(textPrimary())
                .build();

            // threshold slider
            ui.text("settings.threshold.label")
                .size(fieldWidth, 24.0f)
                .text("Recognition Threshold: " + std::to_string(static_cast<int>(state.config.recognition_threshold * 100.0f)) + "%")
                .fontSize(16.0f)
                .lineHeight(22.0f)
                .color(textPrimary())
                .build();

            components::slider(ui, "settings.threshold")
                .theme(components::theme::DarkThemeColors())
                .size(fieldWidth, 32.0f)
                .value(state.config.recognition_threshold)
                .transition(pageTransition())
                .onChange([](float value) {
                    update_state([value](const AppState& s) {
                        return with_threshold(s, value);
                    });
                })
                .build();

            // liveness toggle
            components::toggleSwitch(ui, "settings.liveness")
                .theme(components::theme::DarkThemeColors())
                .size(fieldWidth, 36.0f)
                .checked(state.config.liveness_detection)
                .label("Liveness Detection")
                .onChange([](bool value) {
                    update_state([value](const AppState& s) {
                        return with_liveness(s, value);
                    });
                })
                .build();

            // camera selection
            ui.text("settings.camera.label")
                .size(fieldWidth, 24.0f)
                .text("Camera")
                .fontSize(16.0f)
                .lineHeight(22.0f)
                .color(textPrimary())
                .build();

            auto cameras = su::app::services::backend().enumerate_cameras();
            std::vector<std::string> camNames;
            for (const auto& cam : cameras) {
                camNames.push_back(cam.name);
            }
            if (!camNames.empty()) {
                components::segmented(ui, "settings.camera")
                    .theme(components::theme::DarkThemeColors())
                    .size(fieldWidth, 38.0f)
                    .items(camNames)
                    .selected(state.config.selected_camera)
                    .transition(pageTransition())
                    .onChange([](int index) {
                        update_state([index](const AppState& s) {
                            return with_selected_camera(s, index);
                        });
                    })
                    .build();
            }

            ui.text("settings.diag.title")
                .size(fieldWidth, 30.0f)
                .margin(0.0f, 16.0f, 0.0f, 0.0f)
                .text("Diagnostics")
                .fontSize(24.0f)
                .lineHeight(30.0f)
                .color(textPrimary())
                .build();

            components::button(ui, "settings.diag.toggle")
                .size(200.0f, 48.0f)
                .icon(0xF129)
                .fontSize(16.0f)
                .text(state.show_diagnostics ? "Hide" : "Show")
                .colors(surface(), surfaceSoft(), surfaceActive())
                .textColor(textPrimary())
                .radius(12.0f)
                .border(1.0f, borderColor(0.70f))
                .transition(pageTransition())
                .onClick([] {
                    update_state([](const AppState& s) {
                        return with_show_diagnostics(s, !s.show_diagnostics);
                    });
                })
                .build();

            if (state.show_diagnostics) {
                ui.rect("settings.diag.bg")
                    .size(fieldWidth, 120.0f)
                    .color(surface())
                    .radius(12.0f)
                    .border(1.0f, borderColor())
                    .build();

                ui.text("settings.diag.text")
                    .x(16.0f)
                    .y(16.0f)
                    .size(fieldWidth - 32.0f, 90.0f)
                    .text("Diagnostics info")
                    .fontSize(13.0f)
                    .lineHeight(18.0f)
                    .color(textMuted())
                    .wrap(true)
                    .build();
            }
        });
}

void composePageBody(core::dsl::Ui& ui, float width, float height) {
    switch (su::app::app_state().active_page) {
        case PageId::Dashboard:  composeDashboardPage(ui, width, height); break;
        case PageId::Enrollment: composeEnrollmentPage(ui, width, height); break;
        case PageId::Recognition: composeRecognitionPage(ui, width, height); break;
        case PageId::Settings:   composeSettingsPage(ui, width, height); break;
    }
}

// ---- page body content height ----
float pageBodyContentHeight(float viewportHeight) {
    const auto& state = su::app::app_state();
    switch (state.active_page) {
        case PageId::Dashboard:
            return std::max(viewportHeight, 600.0f);
        case PageId::Enrollment:
            return std::max(viewportHeight, 500.0f);
        case PageId::Recognition:
            return std::max(viewportHeight, 520.0f);
        case PageId::Settings:
            return std::max(viewportHeight, 460.0f);
    }
    return viewportHeight;
}

// ---- content shell ----
void composeContent(core::dsl::Ui& ui, float width, float height) {
    const auto& state = su::app::app_state();
    const auto locale = su::app::i18n::text(su::app::i18n::app_i18n(), state.active_locale, "app.title");
    const auto pageName = su::app::page_name(state.active_page);
    const auto& store = su::app::i18n::app_i18n();

    const float shellWidth = std::max(0.0f, width - 72.0f);
    const float innerWidth = std::max(0.0f, shellWidth - 64.0f);
    const float shellHeight = std::max(0.0f, height - 72.0f);
    const float innerHeight = std::max(0.0f, shellHeight - 64.0f);
    const float headerGap = 26.0f;
    const float bodyHeight = std::max(0.0f, innerHeight - 46.0f - 30.0f - headerGap * 2.0f);
    const float contentHeight = pageBodyContentHeight(bodyHeight);
    const float maxScroll = std::max(0.0f, contentHeight - bodyHeight);
    const bool scrollable = maxScroll > 0.0f;
    const int page = static_cast<int>(state.active_page);
    pageScroll[page] = std::clamp(pageScroll[page], 0.0f, maxScroll);
    const float scrollOffset = pageScroll[page];
    const float scrollWidth = scrollable ? 8.0f : 0.0f;
    const float scrollGap = scrollable ? 16.0f : 0.0f;
    const float bodyContentWidth = std::max(0.0f, innerWidth - scrollWidth - scrollGap);

    const auto titleText = su::app::i18n::text(store, state.active_locale,
        "page." + pageName + ".title");
    const auto descText = su::app::i18n::text(store, state.active_locale,
        "page." + pageName + ".description");

    ui.stack("content.area")
        .size(width, height)
        .content([&] {
            ui.rect("content.bg")
                .size(width, height)
                .color(appBg())
                .build();

            ui.rect("page.shell")
                .size(shellWidth, shellHeight)
                .margin(36.0f)
                .color(surface())
                .radius(26.0f)
                .border(1.0f, borderColor())
                .shadow(30.0f, 0.0f, 16.0f, shadowColor(0.28f))
                .transition(pageTransition())
                .build();

            ui.column("page.content")
                .size(innerWidth, innerHeight)
                .margin(68.0f)
                .gap(headerGap)
                .content([&] {
                    ui.text("page.title")
                        .size(innerWidth, 46.0f)
                        .text(titleText)
                        .fontSize(38.0f)
                        .lineHeight(44.0f)
                        .color(accent())
                        .build();

                    ui.text("page.subtitle")
                        .size(innerWidth, 30.0f)
                        .text(descText)
                        .fontSize(20.0f)
                        .lineHeight(28.0f)
                        .color(textMuted())
                        .build();

                    auto body = ui.stack("page.body")
                        .size(innerWidth, bodyHeight);
                    if (scrollable) {
                        body.onScroll([page](const core::ScrollEvent& event) {
                            pageScroll[page] = std::clamp(
                                pageScroll[page] - static_cast<float>(event.y) * 48.0f,
                                0.0f,
                                pageBodyContentHeight(0.0f) - (0.0f));
                        });
                    }
                    body.content([&] {
                        ui.column("page.body.content")
                            .y(-scrollOffset)
                            .size(bodyContentWidth, contentHeight)
                            .gap(headerGap)
                            .content([&] {
                                composePageBody(ui, bodyContentWidth, contentHeight);
                            })
                            .build();

                        if (scrollable) {
                            components::scroll(ui, "page.body.scroll")
                                .theme(components::theme::DarkThemeColors())
                                .x(std::max(0.0f, innerWidth - scrollWidth))
                                .size(scrollWidth, bodyHeight)
                                .viewport(bodyHeight)
                                .content(contentHeight)
                                .offset(scrollOffset)
                                .zIndex(10)
                                .onChange([page](float value) {
                                    pageScroll[page] = value;
                                })
                                .build();
                        }
                    });
                });
        });
}

} // anonymous namespace

// ============================================================
// Exported entry point
// ============================================================

namespace su::app::ui {

export void render_app_layout(core::dsl::Ui& ui, const core::dsl::Screen& screen) {
    const float contentWidth = std::max(0.0f, screen.width - kSidebarWidth);

    ui.row("root")
        .size(screen.width, screen.height)
        .content([&] {
            composeSidebar(ui, screen.height);
            composeContent(ui, contentWidth, screen.height);
        });

    // Feedback toast
    components::toast(ui, "feedback.toast")
        .theme(components::theme::DarkThemeColors())
        .screen(screen.width, screen.height)
        .visible(toastVisible)
        .duration(3.0f)
        .title("Smile2Unlock")
        .message(feedbackText)
        .onAutoDismiss([] {
            toastVisible = false;
            feedbackText = "Ready";
        })
        .onDismiss([] {
            toastVisible = false;
            feedbackText = "Dismissed";
        })
        .build();
}

} // namespace su::app::ui
