#include "Smile2Unlock/frontend/application.h"
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
    // 主窗口
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1600, 900), ImGuiCond_FirstUseEver);

    ImGui::Begin("Smile2Unlock Control Panel", nullptr, ImGuiWindowFlags_MenuBar);

    // 菜单栏
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                glfwSetWindowShouldClose(window_, true);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                ui_state_.show_demo_window = !ui_state_.show_demo_window;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // 标签页
    if (ImGui::BeginTabBar("MainTabBar")) {
        if (ImGui::BeginTabItem("DLL Injector")) {
            RenderInjectorPanel();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("User Management")) {
            RenderUsersPanel();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Face Recognition")) {
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
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "User Management");
    ImGui::Separator();
    ImGui::Spacing();

    auto users = backend_->GetAllUsers();
    ImGui::Text("Registered Users: %d", (int)users.size());
    ImGui::Spacing();

    // 用户表格
    if (ImGui::BeginTable("UsersTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("Username");
        ImGui::TableSetupColumn("Face Feature");
        ImGui::TableSetupColumn("Actions");
        ImGui::TableHeadersRow();

        for (auto& user : users) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", user.id);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", user.username.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", user.face_feature.c_str());

            ImGui::TableSetColumnIndex(3);
            std::string btn_id = "Delete##" + std::to_string(user.id);
            if (ImGui::Button(btn_id.c_str())) {
                std::string error;
                backend_->DeleteUser(user.id, error);
            }
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 添加用户
    ImGui::Text("Add New User:");
    ImGui::InputText("Username", ui_state_.new_username, sizeof(ui_state_.new_username));
    ImGui::InputText("Password", ui_state_.new_password, sizeof(ui_state_.new_password), ImGuiInputTextFlags_Password);

    if (ImGui::Button("Add User", ImVec2(150, 30))) {
        std::string error;
        if (backend_->AddUser(ui_state_.new_username, ui_state_.new_password, error)) {
            ui_state_.status_message = "✓ User added successfully!";
            memset(ui_state_.new_username, 0, sizeof(ui_state_.new_username));
            memset(ui_state_.new_password, 0, sizeof(ui_state_.new_password));
        } else {
            ui_state_.status_message = "✗ Failed: " + error;
        }
        ui_state_.status_message_timer = 3.0f;
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
