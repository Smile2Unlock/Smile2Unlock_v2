#pragma once

#include "recognizer/recognizer_service.h"
#include "app/core_bridge.h"

#include <expected>
#include <string>
#include <vector>

namespace su::app {

struct AppSnapshot {
    std::string title;
    std::vector<su::recognizer::CameraInfo> cameras;
    CoreConfig config;
    std::string config_path;
    bool slint_enabled = false;
};

class AppController {
public:
    std::expected<AppSnapshot, std::string> load_initial_snapshot();
    std::expected<bool, std::string> evaluate_demo_auth(std::string_view username);
    std::expected<CoreConfig, std::string> load_config_snapshot();
    std::expected<void, std::string> save_config_snapshot(const CoreConfig& config);

private:
    std::string config_path() const;
    su::recognizer::RecognizerService recognizer_{};
};

}  // namespace su::app
