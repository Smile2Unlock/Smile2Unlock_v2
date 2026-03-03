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
        char new_remark[256];
        std::string status_message;
        float status_message_timer;
        
        // 人脸管理
        int selected_user_id;
        bool show_face_capture;
        char face_remark[256];
    };

    UIState ui_state_;
    bool initialized_;
};

} // namespace smile2unlock
