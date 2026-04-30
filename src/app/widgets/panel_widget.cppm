module;

#include <eui/EUINEO.h>
#include <eui/app/DslAppRuntime.h>

export module su.app.widgets.panel;

import std;
import su.app.ui.theme;

export namespace su::app::widgets {

/**
 * @brief 渲染面板组件
 * 
 * 渲染一个基本的面板背景。
 * 
 * @param ui UI上下文
 * @param id 组件ID
 * @param frame 面板区域矩形
 */
void render_panel(EUINEO::UIContext& ui, const std::string& id, const EUINEO::RectFrame& frame);

/**
 * @brief 渲染卡片组件
 * 
 * 渲染一个带圆角和边框的卡片背景。
 * 
 * @param ui UI上下文
 * @param id 组件ID
 * @param frame 卡片区域矩形
 */
void render_card(EUINEO::UIContext& ui, const std::string& id, const EUINEO::RectFrame& frame);

}  // namespace su::app::widgets

namespace su::app::widgets {

/**
 * @brief 渲染面板组件
 */
void render_panel(EUINEO::UIContext& ui, const std::string& id, const EUINEO::RectFrame& frame) {
    ui.panel(id)
        .position(frame.x, frame.y)
        .size(frame.width, frame.height)
        .background(ui::content_bg_color())
        .build();
}

/**
 * @brief 渲染卡片组件
 */
void render_card(EUINEO::UIContext& ui, const std::string& id, const EUINEO::RectFrame& frame) {
    ui.panel(id)
        .position(frame.x, frame.y)
        .size(frame.width, frame.height)
        .background(ui::card_bg_color())
        .rounding(12.0f)
        .border(1.0f, ui::border_light_color())
        .build();
}

}  // namespace su::app::widgets