module;

#include <eui/EUINEO.h>

export module su.app.ui.text;

import std;

export namespace su::app::ui {

/**
 * @brief 文本适配函数
 * 
 * 将文本截断到指定宽度，超出部分用省略号代替。
 * 
 * @param text 原始文本
 * @param max_width 最大宽度（像素）
 * @param font_size 字体大小
 * @return std::string 适配后的文本
 */
std::string fit_text(std::string text, float max_width, float font_size);

}  // namespace su::app::ui

namespace su::app::ui {

std::string fit_text(std::string text, float max_width, float font_size) {
    if (text.empty() || max_width <= 0.0f) {
        return {};
    }

    const float scale = font_size / 24.0f;
    if (EUINEO::Renderer::MeasureTextWidth(text, scale) <= max_width) {
        return text;
    }

    static constexpr std::string_view ellipsis = "...";
    while (!text.empty()) {
        text.pop_back();
        const auto candidate = text + std::string(ellipsis);
        if (EUINEO::Renderer::MeasureTextWidth(candidate, scale) <= max_width) {
            return candidate;
        }
    }
    return std::string(ellipsis);
}

}  // namespace su::app::ui
