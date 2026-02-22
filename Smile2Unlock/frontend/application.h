#pragma once

#include "Smile2Unlock/backend/service.h"
#include <memory>
#include <string>

struct GLFWwindow;

namespace smile2unlock {

class Application {
public:
    Application();
    ~Application();

    bool Initialize();
    void Run();
    void Shutdown();

private:
    void RenderUI();
    void RenderInjectorPanel();
    void RenderUsersPanel();
    void RenderRecognizerPanel();

    std::unique_ptr<BackendService> backend_;
    GLFWwindow* window_;

    struct UIState {
        bool show_demo_window;
        bool injector_tab_active;
        char new_username[64];
        char new_password[64];
        std::string status_message;
        float status_message_timer;
    };

    UIState ui_state_;
    bool initialized_;
};

} // namespace smile2unlock
