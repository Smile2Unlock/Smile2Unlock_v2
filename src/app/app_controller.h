#pragma once

#include "recognizer/recognizer_service.h"

#include <expected>
#include <string>
#include <vector>

namespace su::app {

struct AppSnapshot {
    std::string title;
    std::vector<su::recognizer::CameraInfo> cameras;
    float recognition_threshold = 0.0F;
    bool slint_enabled = false;
};

class AppController {
public:
    std::expected<AppSnapshot, std::string> load_initial_snapshot();
    std::expected<bool, std::string> evaluate_demo_auth(std::string_view username);

private:
    su::recognizer::RecognizerService recognizer_{};
};

}  // namespace su::app

