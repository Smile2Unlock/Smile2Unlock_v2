#include <app/dsl_app.h>
import su.app.ui.layout;

namespace app {

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = {
        "Smile2Unlock",
        "smile2unlock",
        {0.07f, 0.08f, 0.10f, 1.0f},
        1440,
        900,
        false,
        0.0,
    };
    return config;
}

void compose(core::dsl::Ui& ui, const core::dsl::Screen& screen) {
    su::app::ui::render_app_layout(ui, screen);
}

} // namespace app
