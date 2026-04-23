#include <eui/app/DslAppRuntime.h>

#include "app_state.h"
#include "i18n/i18n.h"
#include "pages/page_content.h"
#include "ui/layout.h"

import std;

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::filesystem::path executable_dir() {
#if defined(_WIN32)
    std::array<char, MAX_PATH> buffer{};
    const auto length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return length == 0 ? std::filesystem::current_path() : std::filesystem::path(std::string_view(buffer.data(), length)).parent_path();
#else
    std::array<char, 4096> buffer{};
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
    return length <= 0 ? std::filesystem::current_path() : std::filesystem::path(std::string_view(buffer.data(), static_cast<std::size_t>(length))).parent_path();
#endif
}

void prepare_runtime_working_directory() {
    std::error_code error;
    const auto target_dir = executable_dir();
    if (!target_dir.empty()) {
        std::filesystem::current_path(target_dir, error);
    }
}

}  // namespace

int main() {
    prepare_runtime_working_directory();

    EUINEO::DslAppConfig config;
    config.title = su::app::i18n::text(
        su::app::i18n::app_i18n(),
        su::app::i18n::default_locale(su::app::i18n::app_i18n()),
        "app.title");
    config.width = su::app::ui::kWindowWidth;
    config.height = su::app::ui::kWindowHeight;
    config.pageId = "smile2unlock";
    config.fps = 120;
    config.darkTitleBar = true;

    return EUINEO::RunDslApp(config, [](EUINEO::UIContext& ui, const EUINEO::RectFrame& screen) {
        EUINEO::UseDslDarkTheme(su::app::ui::stage_color());

        ui.panel("stage")
            .position(0.0f, 0.0f)
            .size(screen.width, screen.height)
            .background(su::app::ui::stage_color())
            .gradient(EUINEO::RectGradient::Corners(
                su::app::ui::stage_glow(),
                su::app::ui::stage_color(),
                EUINEO::Color(0.07f, 0.09f, 0.12f, 1.0f),
                EUINEO::Color(0.12f, 0.15f, 0.21f, 1.0f)))
            .build();

        const EUINEO::RectFrame header{
            su::app::ui::kOuterMargin,
            su::app::ui::kOuterMargin,
            screen.width - su::app::ui::kOuterMargin * 2.0f,
            su::app::ui::kHeaderHeight,
        };
        const EUINEO::RectFrame sidebar{
            su::app::ui::kOuterMargin,
            header.y + header.height + su::app::ui::kSectionGap,
            std::min(su::app::ui::kSidebarWidth, screen.width * 0.24f),
            screen.height - header.height - su::app::ui::kOuterMargin * 2.0f - su::app::ui::kSectionGap,
        };
        const EUINEO::RectFrame content{
            sidebar.x + sidebar.width + su::app::ui::kSectionGap,
            sidebar.y,
            std::max(320.0f, screen.width - sidebar.width - su::app::ui::kOuterMargin * 2.0f - su::app::ui::kSectionGap),
            sidebar.height,
        };
        const auto page = su::app::make_page_content(su::app::app_state().active_page);

        su::app::ui::compose_header(ui, header);
        su::app::ui::compose_sidebar(ui, sidebar);
        su::app::ui::compose_main_content(ui, content, page);
    });
}
