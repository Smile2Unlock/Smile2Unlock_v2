module;

#include <eui/app/DslAppRuntime.h>

export module su.app.entry;

import su.app.i18n;
import su.app.runtime;
import su.app.state;
import su.app.ui.layout;
import su.app.ui.theme;

export namespace su::app {

int run_app();

}  // namespace su::app

namespace su::app {

int run_app() {
    runtime::prepare_runtime_working_directory();

    EUINEO::DslAppConfig config;
    config.title = i18n::text(i18n::app_i18n(), i18n::default_locale(i18n::app_i18n()), "app.title");
    config.width = ui::kWindowWidth;
    config.height = ui::kWindowHeight;
    config.pageId = "smile2unlock";
    config.fps = 120;
    config.darkTitleBar = true;

    return EUINEO::RunDslApp(config, [](EUINEO::UIContext& ui_context, const EUINEO::RectFrame& screen) {
        ui::render_app_layout(ui_context, screen, app_state());
    });
}

}  // namespace su::app
