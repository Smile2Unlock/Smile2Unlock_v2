module;

#include <windows.h>
#include <GL/gl.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

export module smile2unlock.application;

import std;
import smile2unlock.service;

struct GLFWwindow;

export namespace smile2unlock {

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
    void PollPreviewStream();
    void RefreshFacePreview();
    void UpdatePreviewTexture(const std::vector<unsigned char>& image_data, int width, int height);
    void ClearPreviewTexture();

    std::unique_ptr<BackendService> backend_;
    GLFWwindow* window_;

    struct UIState {
        bool show_demo_window{};
        bool injector_tab_active{};
        char new_username[64]{};
        char new_password[64]{};
        char new_remark[256]{};
        bool show_edit_user{};
        int editing_user_id{};
        char edit_username[64]{};
        char edit_password[64]{};
        char edit_remark[256]{};
        std::string status_message;
        float status_message_timer{};
        int selected_user_id{};
        bool show_face_capture{};
        char face_remark[256]{};
        bool show_save_face_result{};
        bool save_face_success{};
        GLuint preview_texture{};
        int preview_width{};
        int preview_height{};
        bool has_preview{};
        bool request_preview_refresh{};
        bool camera_enabled{};
        std::string preview_message;
    };

    UIState ui_state_;
    bool initialized_;
};

} // namespace smile2unlock

module :private;

namespace smile2unlock {

constexpr GLint kGlClampToEdge = 0x812F;

Application::Application() : window_(nullptr), initialized_(false) {
    backend_ = std::make_unique<BackendService>();
    char username[256];
    DWORD username_len = sizeof(username);
    if (GetUserNameA(username, &username_len)) {
        strncpy_s(ui_state_.new_username, sizeof(ui_state_.new_username), username, _TRUNCATE);
    }
}

Application::~Application() { Shutdown(); }

bool Application::Initialize() {
    if (!backend_->Initialize()) return false;
    if (!glfwInit()) return false;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    window_ = glfwCreateWindow(1600, 900, "Smile2Unlock Control Panel", nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 24.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    if (io.Fonts->Fonts.Size == 0) {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = 2.5f;
    }
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(2.5f);
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    initialized_ = true;
    return true;
}

void Application::Run() {
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        RenderUI();
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window_, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
    }
}

void Application::Shutdown() {
    if (!initialized_) return;
    ClearPreviewTexture();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
    initialized_ = false;
}

void Application::ClearPreviewTexture() {
    if (ui_state_.preview_texture != 0) {
        glDeleteTextures(1, &ui_state_.preview_texture);
        ui_state_.preview_texture = 0;
    }
    ui_state_.preview_width = 0;
    ui_state_.preview_height = 0;
    ui_state_.has_preview = false;
}

void Application::UpdatePreviewTexture(const std::vector<unsigned char>& image_data, int width, int height) {
    if (image_data.empty() || width <= 0 || height <= 0) {
        ClearPreviewTexture();
        return;
    }

    if (ui_state_.preview_texture == 0) {
        glGenTextures(1, &ui_state_.preview_texture);
    }

    glBindTexture(GL_TEXTURE_2D, ui_state_.preview_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, image_data.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    ui_state_.preview_width = width;
    ui_state_.preview_height = height;
    ui_state_.has_preview = true;
}

void Application::RefreshFacePreview() {
    std::vector<unsigned char> image_data;
    int width = 0;
    int height = 0;
    std::string error_message;
    if (backend_->CapturePreviewFrame(image_data, width, height, error_message)) {
        UpdatePreviewTexture(image_data, width, height);
        ui_state_.preview_message = "预览已更新";
    } else {
        ui_state_.preview_message = error_message.empty() ? "无法获取预览" : error_message;
    }
}

void Application::PollPreviewStream() {
    if (!ui_state_.camera_enabled) {
        return;
    }

    std::vector<unsigned char> image_data;
    int width = 0;
    int height = 0;
    std::string error_message;
    if (backend_->GetLatestCameraPreview(image_data, width, height, error_message)) {
        UpdatePreviewTexture(image_data, width, height);
        ui_state_.preview_message = "摄像头实时预览中";
    } else if (!error_message.empty()) {
        ui_state_.preview_message = error_message;
    }
}

void Application::RenderUI() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Smile2Unlock 控制面板", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("退出", "Alt+F4")) glfwSetWindowShouldClose(window_, true);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("帮助")) {
            if (ImGui::MenuItem("关于")) ui_state_.show_demo_window = !ui_state_.show_demo_window;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (ImGui::BeginTabBar("MainTabBar")) {
        if (ImGui::BeginTabItem("DLL 注入器")) { RenderInjectorPanel(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("用户管理")) { RenderUsersPanel(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("人脸识别")) { RenderRecognizerPanel(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    if (ui_state_.show_save_face_result) {
        ImGui::OpenPopup("保存结果");
        ui_state_.show_save_face_result = false;
    }
    if (ImGui::BeginPopupModal("保存结果", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ui_state_.save_face_success ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1),
                           ui_state_.save_face_success ? "保存成功！" : "保存失败！");
        ImGui::Spacing();
        ImGui::Text("%s", ui_state_.status_message.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("确定", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();
    if (ui_state_.show_demo_window) ImGui::ShowDemoWindow(&ui_state_.show_demo_window);
}

void Application::RenderInjectorPanel() {
    auto status = backend_->GetDllStatus();
    ImGui::TextColored(ImVec4(1,1,0,1), "CredentialProvider DLL Status");
    ImGui::Separator(); ImGui::Spacing();
    ImGui::Text("Injection Status:"); ImGui::SameLine();
    ImGui::TextColored(status.is_injected ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1), status.is_injected ? "INJECTED" : "NOT INJECTED");
    ImGui::Text("Registry Status:"); ImGui::SameLine();
    ImGui::TextColored(status.is_registry_configured ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1), status.is_registry_configured ? "CONFIGURED" : "NOT CONFIGURED");
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::Text("Source DLL:"); ImGui::TextWrapped("%s", status.source_path.c_str());
    ImGui::Text("Target DLL:"); ImGui::TextWrapped("%s", status.target_path.c_str());
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    if (ImGui::Button("Calculate Hashes", ImVec2(200, 30))) status = backend_->GetDllStatus();
    if (!status.source_hash.empty()) {
        ImGui::Text("Source Hash:"); ImGui::TextWrapped("%s", status.source_hash.c_str());
        ImGui::Text("Target Hash:"); ImGui::TextWrapped("%s", status.target_hash.c_str());
        ImGui::Text("Source Version: %s", status.source_version.c_str());
        ImGui::Text("Target Version: %s", status.target_version.c_str());
    }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    if (!status.is_injected || !status.is_registry_configured) {
        if (ImGui::Button("Inject DLL (Requires Admin)", ImVec2(250, 40))) {
            std::string error;
            ui_state_.status_message = backend_->InjectDll(error) ? "✓ DLL injected successfully!" : "✗ Failed: " + error;
            ui_state_.status_message_timer = 5.0f;
        }
    }
    if (ui_state_.status_message_timer > 0.0f) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", ui_state_.status_message.c_str());
        ui_state_.status_message_timer -= ImGui::GetIO().DeltaTime;
    }
}

void Application::RenderUsersPanel() {
    ImGui::TextColored(ImVec4(1,1,0,1), "用户管理");
    ImGui::Separator(); ImGui::Spacing();
    auto users = backend_->GetAllUsers();
    ImGui::Text("已注册用户: %d", static_cast<int>(users.size()));
    ImGui::Spacing();

    if (ImGui::BeginTable("UsersTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("登录用户名");
        ImGui::TableSetupColumn("人脸数量");
        ImGui::TableSetupColumn("备注");
        ImGui::TableSetupColumn("操作");
        ImGui::TableSetupColumn("人脸管理");
        ImGui::TableHeadersRow();
        for (auto& user : users) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", user.id);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", user.username.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d 张", static_cast<int>(user.faces.size()));
            ImGui::TableSetColumnIndex(3); ImGui::TextWrapped("%s", user.remark.c_str());
            ImGui::TableSetColumnIndex(4);
            if (ImGui::Button(("编辑##user_" + std::to_string(user.id)).c_str())) {
                ui_state_.editing_user_id = user.id;
                strncpy_s(ui_state_.edit_username, sizeof(ui_state_.edit_username), user.username.c_str(), _TRUNCATE);
                memset(ui_state_.edit_password, 0, sizeof(ui_state_.edit_password));
                strncpy_s(ui_state_.edit_remark, sizeof(ui_state_.edit_remark), user.remark.c_str(), _TRUNCATE);
                ui_state_.show_edit_user = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(("删除##user_" + std::to_string(user.id)).c_str())) {
                std::string error; backend_->DeleteUser(user.id, error);
            }
            ImGui::TableSetColumnIndex(5);
            if (ImGui::Button(("管理人脸##face_" + std::to_string(user.id)).c_str())) {
                ui_state_.selected_user_id = user.id;
                ui_state_.show_face_capture = true;
                ui_state_.request_preview_refresh = false;
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::Text("添加新用户:");
    ImGui::InputText("CP登录用户名", ui_state_.new_username, sizeof(ui_state_.new_username));
    ImGui::InputText("密码", ui_state_.new_password, sizeof(ui_state_.new_password), ImGuiInputTextFlags_Password);
    ImGui::InputText("备注", ui_state_.new_remark, sizeof(ui_state_.new_remark));
    if (ImGui::Button("添加用户", ImVec2(150, 30))) {
        std::string error;
        if (backend_->AddUser(ui_state_.new_username, ui_state_.new_password, ui_state_.new_remark, error)) {
            ui_state_.status_message = "✓ " + error;
            memset(ui_state_.new_username, 0, sizeof(ui_state_.new_username));
            memset(ui_state_.new_password, 0, sizeof(ui_state_.new_password));
            memset(ui_state_.new_remark, 0, sizeof(ui_state_.new_remark));
        } else {
            ui_state_.status_message = "✗ " + error;
        }
        ui_state_.status_message_timer = 3.0f;
    }

    if (ui_state_.show_edit_user) {
        ImGui::OpenPopup("编辑用户");
        ui_state_.show_edit_user = false;
    }

    if (ImGui::BeginPopupModal("编辑用户", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("修改用户信息");
        ImGui::Separator();
        ImGui::InputText("CP登录用户名##edit", ui_state_.edit_username, sizeof(ui_state_.edit_username));
        ImGui::InputText("新密码##edit", ui_state_.edit_password, sizeof(ui_state_.edit_password), ImGuiInputTextFlags_Password);
        ImGui::InputText("备注##edit", ui_state_.edit_remark, sizeof(ui_state_.edit_remark));
        ImGui::TextDisabled("密码留空表示保持原密码不变");
        ImGui::Spacing();
        if (ImGui::Button("保存", ImVec2(120, 0))) {
            std::string error;
            if (backend_->UpdateUser(ui_state_.editing_user_id, ui_state_.edit_username, ui_state_.edit_password, ui_state_.edit_remark, error)) {
                ui_state_.status_message = "✓ " + error;
                memset(ui_state_.edit_password, 0, sizeof(ui_state_.edit_password));
                ImGui::CloseCurrentPopup();
            } else {
                ui_state_.status_message = "✗ " + error;
            }
            ui_state_.status_message_timer = 3.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(120, 0))) {
            memset(ui_state_.edit_password, 0, sizeof(ui_state_.edit_password));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ui_state_.show_face_capture) {
        ImGui::OpenPopup("人脸管理");
        ui_state_.show_face_capture = false;
    }

    if (ImGui::BeginPopupModal("人脸管理", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto all_users = backend_->GetAllUsers();
        auto user_it = std::find_if(all_users.begin(), all_users.end(), [this](const User& u) { return u.id == ui_state_.selected_user_id; });
        if (user_it != all_users.end()) {
            ImGui::Text("用户: %s", user_it->username.c_str());
            ImGui::Separator(); ImGui::Spacing();
            auto faces = backend_->GetUserFaces(user_it->id);
            ImGui::Text("已录入人脸: %d 张", static_cast<int>(faces.size()));
            ImGui::Spacing();
            if (ImGui::BeginTable("FacesTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("ID"); ImGui::TableSetupColumn("备注"); ImGui::TableSetupColumn("操作"); ImGui::TableHeadersRow();
                for (const auto& face : faces) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%d", face.id);
                    ImGui::TableSetColumnIndex(1); ImGui::TextWrapped("%s", face.remark.c_str());
                    ImGui::TableSetColumnIndex(2);
                    if (ImGui::Button(("删除##face_" + std::to_string(face.id)).c_str())) {
                        std::string error; backend_->DeleteFace(user_it->id, face.id, error);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::Text("录入新人脸:");
            ImGui::InputText("备注##face", ui_state_.face_remark, sizeof(ui_state_.face_remark));
            PollPreviewStream();
            ImGui::TextWrapped("摄像头由 FaceRecognizer 负责，Smile2Unlock 通过 IPC 接收预览图像与人脸特征。");
            if (!ui_state_.camera_enabled) {
                if (ImGui::Button("打开摄像头", ImVec2(160, 30))) {
                    std::string error;
                    if (backend_->StartCameraPreview(error)) {
                        ui_state_.camera_enabled = true;
                        ui_state_.preview_message = error.empty() ? "摄像头已打开" : error;
                    } else {
                        ui_state_.preview_message = error.empty() ? "打开摄像头失败" : error;
                    }
                }
            } else {
                if (ImGui::Button("关闭摄像头", ImVec2(160, 30))) {
                    backend_->StopCameraPreview();
                    ui_state_.camera_enabled = false;
                    ui_state_.preview_message = "摄像头已关闭";
                    ClearPreviewTexture();
                }
            }
            ImGui::SameLine();
            if (!ui_state_.camera_enabled) {
                if (ImGui::Button("单次抓拍", ImVec2(160, 30))) {
                    RefreshFacePreview();
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::Button("单次抓拍", ImVec2(160, 30));
                ImGui::EndDisabled();
            }
            if (!ui_state_.preview_message.empty()) {
                ImGui::TextWrapped("%s", ui_state_.preview_message.c_str());
            }
            ImGui::Spacing();
            if (ui_state_.has_preview && ui_state_.preview_texture != 0) {
                const float max_width = 520.0f;
                const float scale = std::min(max_width / static_cast<float>(ui_state_.preview_width), 1.0f);
                const ImVec2 preview_size(
                    static_cast<float>(ui_state_.preview_width) * scale,
                    static_cast<float>(ui_state_.preview_height) * scale);
                ImGui::Image((ImTextureID)(intptr_t)ui_state_.preview_texture, preview_size);
            } else {
                ImGui::TextDisabled("当前还没有可显示的预览画面");
            }
            if (ImGui::Button("抓拍并录入", ImVec2(180, 30))) {
                std::string error_msg;
                bool success = backend_->CaptureAndAddFace(user_it->id, ui_state_.face_remark, error_msg);
                if (success) {
                    ui_state_.status_message = "人脸特征已成功记录至系统数据库。";
                    ui_state_.save_face_success = true;
                    memset(ui_state_.face_remark, 0, sizeof(ui_state_.face_remark));
                    if (!ui_state_.camera_enabled) {
                        ui_state_.request_preview_refresh = true;
                    }
                } else {
                    ui_state_.status_message = "错误: " + error_msg;
                    ui_state_.save_face_success = false;
                }
                ui_state_.status_message_timer = 3.0f;
                ui_state_.show_save_face_result = true;
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("关闭", ImVec2(120, 0))) {
            backend_->StopCameraPreview();
            ui_state_.camera_enabled = false;
            ui_state_.preview_message.clear();
            ClearPreviewTexture();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ui_state_.status_message_timer > 0.0f) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", ui_state_.status_message.c_str());
    }
}

void Application::RenderRecognizerPanel() {
    ImGui::TextColored(ImVec4(1,1,0,1), "Face Recognition Test");
    ImGui::Separator(); ImGui::Spacing();
    if (ImGui::Button("启动 FaceRecognizer", ImVec2(200, 30))) {
        std::string error;
        ui_state_.status_message = backend_->StartRecognition(error) ? "人脸识别进程已启动" : "启动失败: " + error;
        ui_state_.status_message_timer = 3.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("停止 FaceRecognizer", ImVec2(200, 30))) {
        backend_->StopRecognition();
        ui_state_.status_message = "人脸识别进程已停止";
        ui_state_.status_message_timer = 3.0f;
    }
}

} // namespace smile2unlock
