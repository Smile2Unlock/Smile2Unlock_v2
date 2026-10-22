#include "capture_test_window.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

namespace ui = su::test::ui;
void save(const slint::SharedPixelBuffer<slint::Rgba8Pixel>& pixels, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << pixels.width() << ' ' << pixels.height() << "\n255\n";
    for (const auto& pixel : pixels) {
        const char rgb[]{static_cast<char>(pixel.r), static_cast<char>(pixel.g), static_cast<char>(pixel.b)};
        out.write(rgb, 3);
    }
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    std::ifstream pack(argv[1]);
    const auto strings = nlohmann::json::parse(pack).at("strings");
    auto window = ui::CaptureTestWindow::create();
    window->on_translate([strings](slint::SharedString key, int) {
        return slint::SharedString(strings.value(std::string(key), std::string(key)));
    });
    window->on_translate_value([strings](slint::SharedString key, int, slint::SharedString value) {
        auto text = strings.value(std::string(key), std::string(key));
        if (const auto at = text.find("{value}"); at != std::string::npos) text.replace(at, 7, std::string(value));
        return slint::SharedString(text);
    });
    if (std::string_view(argv[3]) == "populated") {
        slint::SharedPixelBuffer<slint::Rgb8Pixel> pixels(320, 240);
        std::fill(pixels.begin(), pixels.end(), slint::Rgb8Pixel{40, 120, 95});
        const auto photo = slint::Image{pixels};
        window->set_photo(photo);
        window->set_selected(true);
        window->set_records(std::make_shared<slint::VectorModel<ui::CaptureRow>>(
            std::vector<ui::CaptureRow>{{"abc", "2026-10-22 08:00:00 UTC", "mismatch", photo, 0.75F}}));
    }
    window->show();
    slint::Timer timer;
    timer.start(slint::TimerMode::SingleShot, std::chrono::milliseconds{250}, [&] {
        const auto snapshot = window->window().take_snapshot();
        if (!snapshot) std::abort();
        save(*snapshot, argv[2]);
        slint::quit_event_loop();
    });
    slint::run_event_loop();
    std::cout << "capture viewer rendered " << argv[1] << ' ' << argv[3] << '\n';
}
