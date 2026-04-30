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

/**
 * @brief 启动应用程序主循环
 * 
 * 初始化运行时工作目录，配置应用程序参数（标题、尺寸、页面ID、帧率等），
 * 并运行 DSL 应用程序框架。渲染回调函数会调用 UI 布局渲染器来绘制界面。
 * 
 * @return int 应用程序退出码
 */
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
