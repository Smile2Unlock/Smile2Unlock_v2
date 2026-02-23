#include "Smile2Unlock/frontend/application.h"
#include <algorithm>
#include <iostream>
#include <windows.h>
#include <GL/gl.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

namespace smile2unlock {

Application::Application() 
    : window_(nullptr), initialized_(false) {
    backend_ = std::make_unique<BackendService>();
    ui_state_ = {};
    ui_state_.show_demo_window = false;
}

Application::~Application() {
    Shutdown();
}

bool Application::Initialize() {
    std::cout << "Initializing Application..." << std::endl;

    if (!backend_->Initialize()) {
        std::cerr << "Failed to initialize backend service!" << std::endl;
        return false;
    }

    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW!" << std::endl;
        return false;
    }

    // 设置 OpenGL 版本
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // 创建窗口 - 使用合理的大小避免触发游戏模式
    window_ = glfwCreateWindow(1600, 900, "Smile2Unlock Control Panel", nullptr, nullptr);
    if (!window_) {
        std::cerr << "Failed to create GLFW window!" << std::endl;
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1); // V-Sync

    // 初始化 ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    // 加载中文字体避免乱码
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 24.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    
    // 如果字体加载失败，使用默认字体并放大
    if (io.Fonts->Fonts.Size == 0) {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = 2.5f;
    }

    // 设置样式
    ImGui::StyleColorsDark();
    
    // 放大UI元素 (更激进的缩放)
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(2.5f);

    // 初始化 ImGui 后端
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    std::cout << "ImGui initialized successfully!" << std::endl;

    initialized_ = true;
    return true;
}

void Application::Run() {
    std::cout << "Starting main loop..." << std::endl;

    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();

        // 开始新帧
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 渲染 UI
        RenderUI();

        // 渲染
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
    if (initialized_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        if (window_) {
            glfwDestroyWindow(window_);
        }
        glfwTerminate();

        initialized_ = false;
    }
}

void Application::RenderUI() {
    // 使用全窗口模式，不创建额外窗口
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    
    ImGui::Begin("Smile2Unlock 控制面板", nullptr, 
                 ImGuiWindowFlags_NoResize | 
                 ImGuiWindowFlags_NoMove | 
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_MenuBar);

    // 菜单栏
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("退出", "Alt+F4")) {
                glfwSetWindowShouldClose(window_, true);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("帮助")) {
            if (ImGui::MenuItem("关于")) {
                ui_state_.show_demo_window = !ui_state_.show_demo_window;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // 标签页
    if (ImGui::BeginTabBar("MainTabBar")) {
        if (ImGui::BeginTabItem("DLL 注入器")) {
            RenderInjectorPanel();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("用户管理")) {
            RenderUsersPanel();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("人脸识别")) {
            RenderRecognizerPanel();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();

    // Demo 窗口
    if (ui_state_.show_demo_window) {
        ImGui::ShowDemoWindow(&ui_state_.show_demo_window);
    }
}

void Application::RenderInjectorPanel() {
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "CredentialProvider DLL Status");
    ImGui::Separator();
    ImGui::Spacing();

    // 获取状态
    auto status = backend_->GetDllStatus();

    // 显示状态
    ImGui::Text("Injection Status:");
    ImGui::SameLine();
    if (status.is_injected) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "INJECTED");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "NOT INJECTED");
    }

    ImGui::Text("Registry Status:");
    ImGui::SameLine();
    if (status.is_registry_configured) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "CONFIGURED");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "NOT CONFIGURED");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 路径信息
    ImGui::Text("Source DLL:");
    ImGui::TextWrapped("%s", status.source_path.c_str());

    ImGui::Text("Target DLL:");
    ImGui::TextWrapped("%s", status.target_path.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 哈希按钮
    if (ImGui::Button("Calculate Hashes", ImVec2(200, 30))) {
        backend_->CalculateDllHashes();
        status = backend_->GetDllStatus();
    }

    if (!status.source_hash.empty()) {
        ImGui::Text("Source Hash:");
        ImGui::TextWrapped("%s", status.source_hash.c_str());

        ImGui::Text("Target Hash:");
        ImGui::TextWrapped("%s", status.target_hash.c_str());

        ImGui::Text("Source Version: %s", status.source_version.c_str());
        ImGui::Text("Target Version: %s", status.target_version.c_str());

        if (status.hash_match) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Hashes Match!");
        } else if (status.source_hash != "FILE_NOT_FOUND" && status.target_hash != "FILE_NOT_FOUND") {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "✗ Hashes DO NOT Match!");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 注入按钮
    if (!status.is_injected || !status.is_registry_configured) {
        if (ImGui::Button("Inject DLL (Requires Admin)", ImVec2(250, 40))) {
            std::string error;
            if (backend_->InjectDll(error)) {
                ui_state_.status_message = "✓ DLL injected successfully!";
            } else {
                ui_state_.status_message = "✗ Failed: " + error;
            }
            ui_state_.status_message_timer = 5.0f;
        }

        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), 
                          "Note: Requires Administrator privileges");
    } else {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), 
                          "✓ DLL is already injected and configured!");
    }

    // 显示状态消息
    if (ui_state_.status_message_timer > 0.0f) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", ui_state_.status_message.c_str());
        ui_state_.status_message_timer -= ImGui::GetIO().DeltaTime;
    }
}

void Application::RenderUsersPanel() {
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "用户管理");
    ImGui::Separator();
    ImGui::Spacing();

    auto users = backend_->GetAllUsers();
    ImGui::Text("已注册用户: %d", (int)users.size());
    ImGui::Spacing();

    // 用户表格
    if (ImGui::BeginTable("UsersTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("用户名");
        ImGui::TableSetupColumn("人脸数量");
        ImGui::TableSetupColumn("备注");
        ImGui::TableSetupColumn("操作");
        ImGui::TableSetupColumn("人脸管理");
        ImGui::TableHeadersRow();

        for (auto& user : users) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", user.id);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", user.username.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d 张", (int)user.faces.size());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextWrapped("%s", user.remark.c_str());

            ImGui::TableSetColumnIndex(4);
            std::string btn_delete = "删除##user_" + std::to_string(user.id);
            if (ImGui::Button(btn_delete.c_str())) {
                std::string error;
                backend_->DeleteUser(user.id, error);
            }
            
            ImGui::TableSetColumnIndex(5);
            std::string btn_face = "管理人脸##face_" + std::to_string(user.id);
            if (ImGui::Button(btn_face.c_str())) {
                ui_state_.selected_user_id = user.id;
                ui_state_.show_face_capture = true;
            }
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 添加用户
    ImGui::Text("添加新用户:");
    ImGui::InputText("用户名", ui_state_.new_username, sizeof(ui_state_.new_username));
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

    // 人脸管理弹窗
    if (ui_state_.show_face_capture) {
        ImGui::OpenPopup("人脸管理");
        ui_state_.show_face_capture = false;
    }
    
    if (ImGui::BeginPopupModal("人脸管理", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto user_opt = backend_->GetAllUsers();
        auto user_it = std::find_if(user_opt.begin(), user_opt.end(), 
                                     [this](const User& u) { return u.id == ui_state_.selected_user_id; });
        
        if (user_it != user_opt.end()) {
            ImGui::Text("用户: %s", user_it->username.c_str());
            ImGui::Separator();
            ImGui::Spacing();
            
            // 显示已有人脸
            auto faces = backend_->GetUserFaces(user_it->id);
            ImGui::Text("已录入人脸: %d 张", (int)faces.size());
            ImGui::Spacing();
            
            if (ImGui::BeginTable("FacesTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("ID");
                ImGui::TableSetupColumn("备注");
                ImGui::TableSetupColumn("操作");
                ImGui::TableHeadersRow();
                
                for (const auto& face : faces) {
                    ImGui::TableNextRow();
                    
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", face.id);
                    
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", face.remark.c_str());
                    
                    ImGui::TableSetColumnIndex(2);
                    std::string btn_del_face = "删除##face_" + std::to_string(face.id);
                    if (ImGui::Button(btn_del_face.c_str())) {
                        std::string error;
                        backend_->DeleteFace(user_it->id, face.id, error);
                    }
                }
                
                ImGui::EndTable();
            }
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            
            // 添加新人脸
            ImGui::Text("录入新人脸:");
            ImGui::InputText("备注##face", ui_state_.face_remark, sizeof(ui_state_.face_remark));
            
            if (ImGui::Button("打开摄像头", ImVec2(150, 30))) {
                // TODO: 打开摄像头预览
                ui_state_.status_message = "摄像头功能开发中...";
                ui_state_.status_message_timer = 3.0f;
            }
            
            ImGui::SameLine();
            
            if (ImGui::Button("从文件导入", ImVec2(150, 30))) {
                // TODO: 文件选择对话框
                ui_state_.status_message = "文件导入功能开发中...";
                ui_state_.status_message_timer = 3.0f;
            }
        }
        
        ImGui::Spacing();
        if (ImGui::Button("关闭", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }

    if (ui_state_.status_message_timer > 0.0f) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", ui_state_.status_message.c_str());
    }

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), 
                      "Note: Face registration will be added in Phase 2");
}

void Application::RenderRecognizerPanel() {
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Face Recognition Test");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("This panel will show face recognition interface.");
    ImGui::Spacing();

    if (ImGui::Button("Start Recognition (TODO)", ImVec2(200, 30))) {
        std::string error;
        backend_->StartRecognition(error);
        ui_state_.status_message = error;
        ui_state_.status_message_timer = 3.0f;
    }

    ImGui::SameLine();

    if (ImGui::Button("Stop Recognition", ImVec2(200, 30))) {
        backend_->StopRecognition();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), 
                      "Note: Face recognition not yet implemented");
    ImGui::Text("TODO:");
    ImGui::BulletText("Open camera");
    ImGui::BulletText("Detect faces using SeetaFace6");
    ImGui::BulletText("Match with database");
    ImGui::BulletText("Send result via UDP");
}

} // namespace smile2unlock
