#include "app_window.h"
#include "capture_time.h"
#include <nlohmann/json.hpp>

import std;
import su.app.controller;
import su.app.i18n;

namespace su::app {
namespace {
namespace ui = su::app::ui;
using Window = slint::ComponentHandle<ui::AppWindow>;
struct Photo {
    std::string id, time, reason;
    float ratio = 1;
    std::vector<std::uint8_t> jpeg;
};
std::expected<std::vector<Photo>, std::string> load_photos(AppController& controller) {
    const auto bytes = controller.captures("list");
    if (!bytes) return std::unexpected(bytes.error());
    try {
        const auto records = nlohmann::json::parse(bytes->begin(), bytes->end());
        if (!records.is_array() || records.size() > 200) return std::unexpected("invalid captures");
        auto result = std::vector<Photo>{};
        for (const auto& record : records) {
            const auto id = record.at("id").get<std::string>();
            const auto jpeg = controller.captures("fetch", id);
            if (!jpeg) continue; // A concurrent retention/delete can remove a row.
            const auto width = record.at("width").get<unsigned>();
            const auto height = record.at("height").get<unsigned>();
            if (!width || !height || width > 320 || height > 320) continue;
            result.push_back({id,
                capture_local_time(record.at("time").get<std::uint64_t>()),
                record.at("reason").get<std::string>(),
                static_cast<float>(height) / width, *jpeg});
        }
        return result;
    } catch (...) { return std::unexpected("invalid captures"); }
}
struct CaptureUi : std::enable_shared_from_this<CaptureUi> {
    slint::ComponentWeakHandle<ui::AppWindow> window;
    std::shared_ptr<AppController> controller;
    std::shared_ptr<const LanguageCatalog> catalog;
    std::shared_ptr<slint::VectorModel<ui::CaptureRow>> rows = std::make_shared<slint::VectorModel<ui::CaptureRow>>();
    std::atomic<bool> loading{false};
    CaptureUi(const Window& window, std::shared_ptr<AppController> controller,
              std::shared_ptr<const LanguageCatalog> catalog)
        : window(window), controller(std::move(controller)), catalog(std::move(catalog)) {}
    void apply(const std::expected<std::vector<Photo>, std::string>& photos) {
        const auto current = window.lock();
        if (!current) return;
        rows->clear();
        (*current)->set_capture_selected(false);
        (*current)->set_capture_selected_image({});
        (*current)->set_capture_status(photos ? slint::SharedString{} : slint::SharedString(
            catalog->translate((*current)->get_language_index(), "capture.error")));
        if (!photos) return;
        for (const auto& photo : *photos) {
            rows->push_back(ui::CaptureRow{slint::SharedString(photo.id), slint::SharedString(photo.time),
                slint::SharedString(photo.reason), slint::Image::load_from_data(photo.jpeg, "jpg"), photo.ratio});
        }
    }
    void refresh(std::string op = "list", std::string id = {}) {
        if (loading.exchange(true)) return;
        if (const auto current = window.lock()) {
            (*current)->set_capture_status(slint::SharedString(catalog->translate(
                (*current)->get_language_index(), "capture.loading")));
        }
        const auto self = shared_from_this();
        std::thread([self, op = std::move(op), id = std::move(id)] {
            auto result = std::expected<std::vector<Photo>, std::string>{std::unexpected("capture failure")};
            try {
                const auto changed = self->controller->captures(op, id);
                result = changed ? load_photos(*self->controller) : std::unexpected(changed.error());
            } catch (...) { /* show a localized failure, never log image data */ }
            slint::invoke_from_event_loop([self, result = std::move(result)] {
                self->loading = false;
                self->apply(result);
            });
        }).detach();
    }
    void open(std::string_view id) {
        const auto current = window.lock();
        if (!current) return;
        for (std::size_t i = 0; i < rows->row_count(); ++i) {
            const auto row = rows->row_data(i);
            if (row && std::string_view(row->id) == id) {
                (*current)->set_capture_selected_image(row->thumbnail);
                (*current)->set_capture_selected(true);
                break;
            }
        }
    }
};
}
void setup_captures(const Window& window, const std::shared_ptr<AppController>& controller,
                    const std::shared_ptr<const LanguageCatalog>& catalog) {
    const auto state = std::make_shared<CaptureUi>(window, controller, catalog);
    window->set_captures(state->rows);
    window->on_capture_refresh([state] { state->refresh(); });
    window->on_capture_delete([state](slint::SharedString id) { state->refresh("delete", std::string(id)); });
    window->on_capture_delete_all([state] { state->refresh("delete_all"); });
    window->on_capture_open([state](slint::SharedString id) { state->open(std::string_view(id)); });
}
}
