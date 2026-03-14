#pragma once

#include "Smile2Unlock/backend/service.h"
#include <memory>
#include <string>

struct GLFWwindow;
class CameraCapture;

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
    
    // 摄像头管理
    void OpenCamera(int camera_index = 0);
    void CloseCamera();
    void UpdateCameraFrame();
    void CaptureAndExtractFeature();

    std::unique_ptr<BackendService> backend_;
    std::unique_ptr<CameraCapture> camera_capture_;
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
        
        // 摄像头状态
        bool camera_active;
        int camera_index;
        unsigned char* camera_frame_data;  // OpenGL 纹理数据
        int camera_frame_width;
        int camera_frame_height;
        unsigned int camera_texture_id;
    };

    UIState ui_state_;
    bool initialized_;
};

} // namespace smile2unlock
