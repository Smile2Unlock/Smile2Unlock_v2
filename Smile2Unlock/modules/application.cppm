module;

#include <windows.h>
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <secext.h>
#include <GL/gl.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

export module smile2unlock.application;

import std;
import smile2unlock.models;
import smile2unlock.service;

export namespace smile2unlock {

class Application {
public:
    Application();
    ~Application();

    bool Initialize();
    void Run();
    void Shutdown();
    
    // 设置后端服务（本地或远程）
    void SetBackend(std::unique_ptr<IBackendService> backend) {
        backend_ = std::move(backend);
    }
    
    // 是否使用远程后端
    bool IsUsingRemoteBackend() const {
        return is_remote_backend_;
    }
    
    void SetRemoteBackendFlag(bool is_remote) {
        is_remote_backend_ = is_remote;
    }

private:
    void RenderUI();
    void RenderShell();
    void RenderTitleBar();
    void RenderSidebar();
    void RenderOverview();
    void RenderInjectorPanel();
    void RenderUsersPanel();
    void RenderRecognizerPanel();
    void PollPreviewStream();
    void RefreshFacePreview();
    void UpdatePreviewTexture(const std::vector<unsigned char>& image_data, int width, int height);
    void ClearPreviewTexture();
    void ApplyNordStyle();
    void RenderSectionHeader(const char* eyebrow, const char* title, const char* body = nullptr);
    void RenderInfoRow(const char* label, const std::string& value, const ImVec4& value_color = ImVec4(0, 0, 0, 0));
    void RenderStatTile(const char* id, const char* label, const std::string& value, const std::string& hint, const ImVec4& accent, float width);
    bool RenderNavItem(const char* id, const char* label, const char* hint, bool selected);
    bool RenderWindowControlButton(int icon, const ImVec4& button_color, const ImVec4& hover_color, float width = 68.0f);
    void RenderStatusMessage();
    void BeginPanelCard(const char* id, float height = 0.0f);
    void EndPanelCard();

    std::unique_ptr<IBackendService> backend_;
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
        int active_panel{};
        
        // 缓存的 DLL 状态
        DllStatus cached_dll_status;
        float dll_status_refresh_timer{-1.0f};  // -1 表示需要立即刷新
    };

    UIState ui_state_;
    bool initialized_;
    bool is_remote_backend_{false};
};

} // namespace smile2unlock

module :private;

namespace smile2unlock {

namespace NordColors {
    constexpr ImVec4 Nord0 = ImVec4(46.0f / 255.0f, 52.0f / 255.0f, 64.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord1 = ImVec4(59.0f / 255.0f, 66.0f / 255.0f, 82.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord2 = ImVec4(67.0f / 255.0f, 76.0f / 255.0f, 94.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord3 = ImVec4(76.0f / 255.0f, 86.0f / 255.0f, 106.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord4 = ImVec4(216.0f / 255.0f, 222.0f / 255.0f, 233.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord5 = ImVec4(229.0f / 255.0f, 233.0f / 255.0f, 240.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord6 = ImVec4(236.0f / 255.0f, 239.0f / 255.0f, 244.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord7 = ImVec4(143.0f / 255.0f, 188.0f / 255.0f, 187.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord8 = ImVec4(136.0f / 255.0f, 192.0f / 255.0f, 208.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord9 = ImVec4(129.0f / 255.0f, 161.0f / 255.0f, 193.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord10 = ImVec4(94.0f / 255.0f, 129.0f / 255.0f, 172.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord11 = ImVec4(191.0f / 255.0f, 97.0f / 255.0f, 106.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord12 = ImVec4(208.0f / 255.0f, 135.0f / 255.0f, 112.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord13 = ImVec4(235.0f / 255.0f, 203.0f / 255.0f, 139.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord14 = ImVec4(163.0f / 255.0f, 190.0f / 255.0f, 140.0f / 255.0f, 1.0f);
    constexpr ImVec4 Nord15 = ImVec4(180.0f / 255.0f, 142.0f / 255.0f, 173.0f / 255.0f, 1.0f);
}

using namespace NordColors;

enum PanelIndex {
    PanelInjector = 0,
    PanelUsers = 1,
    PanelRecognizer = 2
};

enum WindowControlIcon {
    WindowControlMinimize = 0,
    WindowControlMaximize = 1,
    WindowControlRestore = 2,
    WindowControlClose = 3
};

constexpr GLint kGlClampToEdge = 0x812F;

ImVec4 WithAlpha(const ImVec4& color, const float alpha) {
    return ImVec4(color.x, color.y, color.z, alpha);
}

bool TryCopyWideStringToUtf8(_In_z_ const wchar_t* source,
                             _Out_writes_z_(target_size) char* target,
                             const size_t target_size) {
    if (source == nullptr || target == nullptr || target_size == 0) {
        return false;
    }

    const int required_size = WideCharToMultiByte(
        CP_UTF8, 0, source, -1, nullptr, 0, nullptr, nullptr);
    if (required_size <= 0 || static_cast<size_t>(required_size) > target_size) {
        return false;
    }

    return WideCharToMultiByte(
               CP_UTF8, 0, source, -1, target, required_size, nullptr, nullptr) > 0;
}

bool TryGetDefaultCpUsername(_Out_writes_z_(buffer_size) char* buffer,
                             const size_t buffer_size) {
    if (buffer == nullptr || buffer_size == 0) {
        return false;
    }

    buffer[0] = '\0';

    ULONG upn_length = 0;
    if (!GetUserNameExW(NameUserPrincipal, nullptr, &upn_length) &&
        GetLastError() == ERROR_MORE_DATA && upn_length > 1) {
        std::wstring upn(static_cast<size_t>(upn_length), L'\0');
        if (GetUserNameExW(NameUserPrincipal, upn.data(), &upn_length) && upn_length > 0) {
            if (TryCopyWideStringToUtf8(upn.c_str(), buffer, buffer_size)) {
                return true;
            }
        }
    }

    DWORD username_length = 0;
    if (!GetUserNameW(nullptr, &username_length) &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER && username_length > 1) {
        std::wstring username(static_cast<size_t>(username_length), L'\0');
        if (GetUserNameW(username.data(), &username_length) && username_length > 0) {
            return TryCopyWideStringToUtf8(username.c_str(), buffer, buffer_size);
        }
    }

    return false;
}

Application::Application() : window_(nullptr), initialized_(false) {
    // 后端由外部设置，不再在此处创建
    char username[256]{};
    if (TryGetDefaultCpUsername(username, sizeof(username))) {
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
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
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
    ApplyNordStyle();
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
        glClearColor(Nord0.x, Nord0.y, Nord0.z, 1.0f);
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
    if (ui_state_.dll_status_refresh_timer < 0 || ui_state_.dll_status_refresh_timer > 2.0f) {
        ui_state_.cached_dll_status = backend_->GetDllStatus();
        ui_state_.dll_status_refresh_timer = 0.0f;
    }
    ui_state_.dll_status_refresh_timer += ImGui::GetIO().DeltaTime;

    RenderShell();

    if (ui_state_.show_save_face_result) {
        ImGui::OpenPopup("保存结果");
        ui_state_.show_save_face_result = false;
    }
    if (ImGui::BeginPopupModal("保存结果", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        RenderSectionHeader("RESULT", ui_state_.save_face_success ? "保存成功" : "保存失败");
        RenderInfoRow("详情", ui_state_.status_message, ui_state_.save_face_success ? Nord14 : Nord11);
        ImGui::Spacing();
        if (ImGui::Button("确定", ImVec2(160, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void Application::RenderShell() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Smile2Unlock 控制面板", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus);

    RenderTitleBar();
    ImGui::Spacing();

    const float content_height = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("Sidebar", ImVec2(320.0f, content_height), true);
    RenderSidebar();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("MainContent", ImVec2(0, content_height), false);
    RenderOverview();
    ImGui::Spacing();

    if (ui_state_.active_panel == PanelInjector) {
        RenderInjectorPanel();
    } else if (ui_state_.active_panel == PanelUsers) {
        RenderUsersPanel();
    } else {
        RenderRecognizerPanel();
    }

    RenderStatusMessage();
    ImGui::EndChild();

    ImGui::End();
    if (ui_state_.show_demo_window) ImGui::ShowDemoWindow(&ui_state_.show_demo_window);
}

void Application::RenderTitleBar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, WithAlpha(Nord1, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(Nord3, 0.65f));
    ImGui::BeginChild("TitleBar", ImVec2(0, 118.0f), true, ImGuiWindowFlags_NoScrollbar);

    const bool is_maximized = glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE;
    const char* current_panel =
        ui_state_.active_panel == PanelInjector ? "DLL 部署" :
        ui_state_.active_panel == PanelUsers ? "用户管理" : "识别服务";

    ImGui::PushStyleColor(ImGuiCol_Text, Nord6);
    ImGui::TextUnformatted("Smile2Unlock");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.72f));
    ImGui::TextUnformatted("Control Surface");
    ImGui::PopStyleColor();

    const float button_width = 52.0f;
    const float button_spacing = 8.0f;
    const float total_buttons_width = button_width * 3.0f + button_spacing * 2.0f;
    ImGui::SetCursorPos(ImVec2(
        ImGui::GetWindowWidth() - total_buttons_width - ImGui::GetStyle().WindowPadding.x,
        18.0f
    ));
    if (RenderWindowControlButton(WindowControlMinimize, WithAlpha(Nord3, 0.85f), WithAlpha(Nord9, 0.78f), button_width)) {
        glfwIconifyWindow(window_);
    }
    ImGui::SameLine(0.0f, button_spacing);
    if (RenderWindowControlButton(is_maximized ? WindowControlRestore : WindowControlMaximize, WithAlpha(Nord10, 0.80f), WithAlpha(Nord8, 0.92f), button_width)) {
        if (is_maximized) {
            glfwRestoreWindow(window_);
        } else {
            glfwMaximizeWindow(window_);
        }
    }
    ImGui::SameLine(0.0f, button_spacing);
    if (RenderWindowControlButton(WindowControlClose, WithAlpha(Nord11, 0.78f), WithAlpha(Nord11, 0.95f), button_width)) {
        glfwSetWindowShouldClose(window_, true);
    }

    ImGui::SetCursorPosY(58.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.78f));
    ImGui::TextWrapped("当前分区: %s | 后端: %s | 窗口: %s",
        current_panel,
        is_remote_backend_ ? "Remote Service" : "Standalone",
        is_maximized ? "最大化" : "窗口模式");
    ImGui::PopStyleColor();

    ImGui::SetCursorPosY(82.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord9, 0.88f));
    ImGui::TextWrapped("%s",
        ui_state_.cached_dll_status.is_injected
            ? "CredentialProvider 已部署，可继续检查用户与采集流程。"
            : "CredentialProvider 尚未部署，建议先完成 DLL 注入与注册表修复。");
    ImGui::PopStyleColor();

    const bool title_bar_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool mouse_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool mouse_double_clicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool item_hovered = ImGui::IsAnyItemHovered();
    const bool item_active = ImGui::IsAnyItemActive();
    if (title_bar_hovered && !item_hovered && !item_active) {
        if (mouse_double_clicked) {
            if (is_maximized) {
                glfwRestoreWindow(window_);
            } else {
                glfwMaximizeWindow(window_);
            }
        } else if (mouse_clicked) {
            HWND hwnd = glfwGetWin32Window(window_);
            if (hwnd != nullptr) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

void Application::RenderSidebar() {
    RenderSectionHeader("NAVIGATION", "控制区", "用分区导航替代挤在一起的标签页。");
    if (RenderNavItem("NavInjector", "DLL 部署", "检查注入与注册表状态", ui_state_.active_panel == PanelInjector)) {
        ui_state_.active_panel = PanelInjector;
    }
    if (RenderNavItem("NavUsers", "用户管理", "浏览账号、人脸和录入流程", ui_state_.active_panel == PanelUsers)) {
        ui_state_.active_panel = PanelUsers;
    }
    if (RenderNavItem("NavRecognizer", "识别服务", "启动或停止 FaceRecognizer", ui_state_.active_panel == PanelRecognizer)) {
        ui_state_.active_panel = PanelRecognizer;
    }

    ImGui::Spacing();
    BeginPanelCard("SidebarState");
    RenderSectionHeader("STATUS", "当前环境");
    RenderInfoRow("后端模式", is_remote_backend_ ? "远程 Service" : "本地进程");
    RenderInfoRow("DLL 注入", ui_state_.cached_dll_status.is_injected ? "已完成" : "未完成", ui_state_.cached_dll_status.is_injected ? Nord14 : Nord13);
    RenderInfoRow("摄像头预览", ui_state_.camera_enabled ? "实时传输中" : "未启用", ui_state_.camera_enabled ? Nord8 : Nord3);
    EndPanelCard();
}

void Application::RenderOverview() {
    auto users = backend_->GetAllUsers();
    const int face_count = std::accumulate(users.begin(), users.end(), 0, [](const int total, const User& user) {
        return total + static_cast<int>(user.faces.size());
    });

    RenderSectionHeader("OVERVIEW", "系统概览", "把短胶囊改成带说明的统计块，数值和含义一起展示。");
    const float total_width = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float tile_width = (total_width - gap * 2.0f) / 3.0f;

    RenderStatTile("OverviewUsers", "用户数量", std::to_string(users.size()), "已登记账号", Nord8, tile_width);
    ImGui::SameLine(0.0f, gap);
    RenderStatTile("OverviewFaces", "人脸样本", std::to_string(face_count), "数据库内总记录", Nord14, tile_width);
    ImGui::SameLine(0.0f, gap);
    RenderStatTile("OverviewDll", "部署状态", ui_state_.cached_dll_status.is_registry_configured ? "Ready" : "Pending", "CredentialProvider", ui_state_.cached_dll_status.is_registry_configured ? Nord9 : Nord13, tile_width);
}

void Application::RenderInjectorPanel() {
    const auto& status = ui_state_.cached_dll_status;

    BeginPanelCard("InjectorPanel");
    RenderSectionHeader("DEPLOYMENT", "CredentialProvider DLL", "用明细行而不是胶囊展示长路径、版本和哈希。");
    RenderInfoRow("注入状态", status.is_injected ? "已注入" : "未注入", status.is_injected ? Nord14 : Nord11);
    RenderInfoRow("注册表状态", status.is_registry_configured ? "已配置" : "未配置", status.is_registry_configured ? Nord14 : Nord13);
    RenderInfoRow("源 DLL 路径", status.source_path);
    RenderInfoRow("目标 DLL 路径", status.target_path);
    if (!status.source_hash.empty()) {
        RenderInfoRow("源 DLL 哈希", status.source_hash, Nord7);
        RenderInfoRow("目标 DLL 哈希", status.target_hash, Nord7);
        RenderInfoRow("源 DLL 版本", status.source_version);
        RenderInfoRow("目标 DLL 版本", status.target_version);
    }

    ImGui::Spacing();
    if (ImGui::Button("刷新部署信息", ImVec2(220, 0))) {
        ui_state_.cached_dll_status = backend_->GetDllStatus();
        ui_state_.dll_status_refresh_timer = 0.0f;
    }
    ImGui::SameLine();
    if (!status.is_injected || !status.is_registry_configured) {
        ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(Nord12, 0.82f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(Nord12, 0.96f));
        if (ImGui::Button("注入 DLL 并修复注册表", ImVec2(280, 0))) {
            std::string error;
            ui_state_.status_message = backend_->InjectDll(error) ? "DLL 部署已完成。" : "部署失败: " + error;
            ui_state_.status_message_timer = 5.0f;
            ui_state_.dll_status_refresh_timer = -1.0f;
        }
        ImGui::PopStyleColor(2);
    }
    EndPanelCard();
}

void Application::RenderUsersPanel() {
    BeginPanelCard("UsersPanel");
    RenderSectionHeader("USERS", "账号与人脸", "列表保留表格，新增和采集改成独立区块。");
    auto users = backend_->GetAllUsers();
    RenderInfoRow("已注册用户", std::to_string(static_cast<int>(users.size())) + " 位", Nord8);

    if (ImGui::BeginTable("UsersTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("登录用户名", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("人脸数量", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("备注", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("人脸管理", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableHeadersRow();
        for (auto& user : users) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", user.id);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", user.username.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d 张", static_cast<int>(user.faces.size()));
            ImGui::TableSetColumnIndex(3); ImGui::TextWrapped("%s", user.remark.c_str());
            ImGui::TableSetColumnIndex(4);
            if (ImGui::Button(("编辑##user_" + std::to_string(user.id)).c_str(), ImVec2(92.0f, 0.0f))) {
                ui_state_.editing_user_id = user.id;
                strncpy_s(ui_state_.edit_username, sizeof(ui_state_.edit_username), user.username.c_str(), _TRUNCATE);
                memset(ui_state_.edit_password, 0, sizeof(ui_state_.edit_password));
                strncpy_s(ui_state_.edit_remark, sizeof(ui_state_.edit_remark), user.remark.c_str(), _TRUNCATE);
                ui_state_.show_edit_user = true;
            }
            ImGui::SameLine(0.0f, 10.0f);
            if (ImGui::Button(("删除##user_" + std::to_string(user.id)).c_str(), ImVec2(92.0f, 0.0f))) {
                std::string error; backend_->DeleteUser(user.id, error);
            }
            ImGui::TableSetColumnIndex(5);
            if (ImGui::Button(("管理人脸##face_" + std::to_string(user.id)).c_str(), ImVec2(148.0f, 0.0f))) {
                ui_state_.selected_user_id = user.id;
                ui_state_.show_face_capture = true;
                ui_state_.request_preview_refresh = false;
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    RenderSectionHeader("CREATE", "添加新用户");
    ImGui::InputText("CP登录用户名", ui_state_.new_username, sizeof(ui_state_.new_username));
    ImGui::InputText("密码", ui_state_.new_password, sizeof(ui_state_.new_password), ImGuiInputTextFlags_Password);
    ImGui::InputText("备注", ui_state_.new_remark, sizeof(ui_state_.new_remark));
    if (ImGui::Button("添加用户", ImVec2(180, 0))) {
        std::string error;
        if (backend_->AddUser(ui_state_.new_username, ui_state_.new_password, ui_state_.new_remark, error)) {
            ui_state_.status_message = error;
            memset(ui_state_.new_username, 0, sizeof(ui_state_.new_username));
            memset(ui_state_.new_password, 0, sizeof(ui_state_.new_password));
            memset(ui_state_.new_remark, 0, sizeof(ui_state_.new_remark));
        } else {
            ui_state_.status_message = "添加失败: " + error;
        }
        ui_state_.status_message_timer = 3.0f;
    }

    if (ui_state_.show_edit_user) {
        ImGui::OpenPopup("编辑用户");
        ui_state_.show_edit_user = false;
    }

    if (ImGui::BeginPopupModal("编辑用户", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        RenderSectionHeader("EDIT", "修改用户信息");
        ImGui::InputText("CP登录用户名##edit", ui_state_.edit_username, sizeof(ui_state_.edit_username));
        ImGui::InputText("新密码##edit", ui_state_.edit_password, sizeof(ui_state_.edit_password), ImGuiInputTextFlags_Password);
        ImGui::InputText("备注##edit", ui_state_.edit_remark, sizeof(ui_state_.edit_remark));
        ImGui::TextDisabled("密码留空表示保持原密码不变");
        ImGui::Spacing();
        if (ImGui::Button("保存", ImVec2(120, 0))) {
            std::string error;
            if (backend_->UpdateUser(ui_state_.editing_user_id, ui_state_.edit_username, ui_state_.edit_password, ui_state_.edit_remark, error)) {
                ui_state_.status_message = error;
                memset(ui_state_.edit_password, 0, sizeof(ui_state_.edit_password));
                ImGui::CloseCurrentPopup();
            } else {
                ui_state_.status_message = "更新失败: " + error;
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
            RenderSectionHeader("CAPTURE", user_it->username.c_str(), "人脸列表和预览区分开展示，避免信息堆在一行里。");
            auto faces = backend_->GetUserFaces(user_it->id);
            RenderInfoRow("已录入人脸", std::to_string(static_cast<int>(faces.size())) + " 张", Nord8);
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
            ImGui::Spacing();
            RenderSectionHeader("NEW FACE", "录入新人脸");
            ImGui::InputText("备注##face", ui_state_.face_remark, sizeof(ui_state_.face_remark));
            PollPreviewStream();
            RenderInfoRow("预览说明", "摄像头由 FaceRecognizer 负责，Smile2Unlock 通过 IPC 接收预览图像与人脸特征。");
            if (!ui_state_.camera_enabled) {
                if (ImGui::Button("打开摄像头", ImVec2(160, 0))) {
                    std::string error;
                    if (backend_->StartCameraPreview(error)) {
                        ui_state_.camera_enabled = true;
                        ui_state_.preview_message = error.empty() ? "摄像头已打开" : error;
                    } else {
                        ui_state_.preview_message = error.empty() ? "打开摄像头失败" : error;
                    }
                }
            } else {
                if (ImGui::Button("关闭摄像头", ImVec2(160, 0))) {
                    backend_->StopCameraPreview();
                    ui_state_.camera_enabled = false;
                    ui_state_.preview_message = "摄像头已关闭";
                    ClearPreviewTexture();
                }
            }
            ImGui::SameLine();
            if (!ui_state_.camera_enabled) {
                if (ImGui::Button("单次抓拍", ImVec2(160, 0))) {
                    RefreshFacePreview();
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::Button("单次抓拍", ImVec2(160, 0));
                ImGui::EndDisabled();
            }
            if (!ui_state_.preview_message.empty()) {
                RenderInfoRow("预览状态", ui_state_.preview_message, ui_state_.camera_enabled ? Nord8 : Nord13);
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
                RenderInfoRow("预览画面", "当前还没有可显示的预览画面");
            }
            if (ImGui::Button("抓拍并录入", ImVec2(180, 0))) {
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
    EndPanelCard();
}

void Application::RenderRecognizerPanel() {
    BeginPanelCard("RecognizerPanel");
    RenderSectionHeader("SERVICE", "FaceRecognizer", "服务控制区保持最少元素，只呈现运行结果和操作。");
    RenderInfoRow("摄像头预览", ui_state_.camera_enabled ? "活跃" : "未启动", ui_state_.camera_enabled ? Nord8 : Nord3);
    if (ImGui::Button("启动 FaceRecognizer", ImVec2(240, 0))) {
        std::string error;
        ui_state_.status_message = backend_->StartRecognition(error) ? "人脸识别进程已启动" : "启动失败: " + error;
        ui_state_.status_message_timer = 3.0f;
    }
    ImGui::SameLine(0.0f, 12.0f);
    if (ImGui::Button("停止 FaceRecognizer", ImVec2(240, 0))) {
        backend_->StopRecognition();
        ui_state_.status_message = "人脸识别进程已停止";
        ui_state_.status_message_timer = 3.0f;
    }
    EndPanelCard();
}

void Application::ApplyNordStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(2.0f);
    style.WindowPadding = ImVec2(20.0f, 20.0f);
    style.FramePadding = ImVec2(16.0f, 10.0f);
    style.ItemSpacing = ImVec2(14.0f, 14.0f);
    style.ItemInnerSpacing = ImVec2(10.0f, 10.0f);
    style.CellPadding = ImVec2(12.0f, 10.0f);
    style.ScrollbarSize = 22.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 10.0f;
    style.PopupRounding = 12.0f;
    style.TabRounding = 10.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = Nord0;
    colors[ImGuiCol_ChildBg] = WithAlpha(Nord1, 0.92f);
    colors[ImGuiCol_PopupBg] = WithAlpha(Nord1, 0.98f);
    colors[ImGuiCol_Border] = WithAlpha(Nord3, 0.55f);
    colors[ImGuiCol_FrameBg] = WithAlpha(Nord1, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = WithAlpha(Nord2, 1.0f);
    colors[ImGuiCol_FrameBgActive] = WithAlpha(Nord2, 1.0f);
    colors[ImGuiCol_TitleBg] = Nord1;
    colors[ImGuiCol_TitleBgActive] = Nord1;
    colors[ImGuiCol_Text] = Nord4;
    colors[ImGuiCol_TextDisabled] = Nord3;
    colors[ImGuiCol_Button] = WithAlpha(Nord10, 0.82f);
    colors[ImGuiCol_ButtonHovered] = WithAlpha(Nord9, 0.95f);
    colors[ImGuiCol_ButtonActive] = Nord8;
    colors[ImGuiCol_Header] = WithAlpha(Nord2, 1.0f);
    colors[ImGuiCol_HeaderHovered] = WithAlpha(Nord9, 0.35f);
    colors[ImGuiCol_HeaderActive] = WithAlpha(Nord9, 0.45f);
    colors[ImGuiCol_Separator] = WithAlpha(Nord3, 0.50f);
    colors[ImGuiCol_Tab] = WithAlpha(Nord1, 1.0f);
    colors[ImGuiCol_TabHovered] = WithAlpha(Nord2, 1.0f);
    colors[ImGuiCol_TabActive] = WithAlpha(Nord10, 0.90f);
    colors[ImGuiCol_TableHeaderBg] = WithAlpha(Nord1, 1.0f);
    colors[ImGuiCol_TableBorderStrong] = WithAlpha(Nord3, 0.65f);
    colors[ImGuiCol_TableBorderLight] = WithAlpha(Nord3, 0.35f);
    colors[ImGuiCol_TableRowBgAlt] = WithAlpha(Nord1, 0.50f);
    colors[ImGuiCol_ScrollbarBg] = WithAlpha(Nord1, 0.80f);
    colors[ImGuiCol_ScrollbarGrab] = WithAlpha(Nord3, 0.85f);
    colors[ImGuiCol_ScrollbarGrabHovered] = WithAlpha(Nord9, 0.90f);
    colors[ImGuiCol_ScrollbarGrabActive] = Nord8;
    colors[ImGuiCol_CheckMark] = Nord8;
    colors[ImGuiCol_SliderGrab] = Nord8;
    colors[ImGuiCol_SliderGrabActive] = Nord9;
    colors[ImGuiCol_TextSelectedBg] = WithAlpha(Nord8, 0.30f);
}

void Application::RenderSectionHeader(const char* eyebrow, const char* title, const char* body) {
    if (eyebrow && eyebrow[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, Nord9);
        ImGui::TextUnformatted(eyebrow);
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, Nord6);
    ImGui::TextWrapped("%s", title);
    ImGui::PopStyleColor();
    if (body && body[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.78f));
        ImGui::TextWrapped("%s", body);
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

void Application::RenderInfoRow(const char* label, const std::string& value, const ImVec4& value_color) {
    const float wrap_width = std::max(ImGui::GetContentRegionAvail().x - 28.0f, 180.0f);
    const float text_height = ImGui::CalcTextSize(value.c_str(), nullptr, false, wrap_width).y;
    const float row_height = std::max(84.0f, 48.0f + text_height + ImGui::GetStyle().WindowPadding.y * 0.7f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, WithAlpha(Nord0, 0.28f));
    ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(Nord3, 0.25f));
    ImGui::BeginChild(label, ImVec2(0, row_height), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.62f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    const ImVec4 color = (value_color.w == 0.0f && value_color.x == 0.0f && value_color.y == 0.0f && value_color.z == 0.0f) ? Nord6 : value_color;
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", value.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

void Application::RenderStatTile(const char* id, const char* label, const std::string& value, const std::string& hint, const ImVec4& accent, float width) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, WithAlpha(Nord1, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(accent, 0.42f));
    ImGui::BeginChild(id, ImVec2(width, 136.0f), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.65f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::TextWrapped("%s", value.c_str());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.75f));
    ImGui::TextWrapped("%s", hint.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

bool Application::RenderNavItem(const char* id, const char* label, const char* hint, bool selected) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? WithAlpha(Nord10, 0.36f) : WithAlpha(Nord1, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, selected ? WithAlpha(Nord8, 0.65f) : WithAlpha(Nord3, 0.30f));
    ImGui::BeginChild(id, ImVec2(0, 98.0f), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? Nord6 : Nord4);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(Nord4, 0.72f));
    ImGui::TextWrapped("%s", hint);
    ImGui::PopStyleColor();
    const bool hovered = ImGui::IsWindowHovered();
    const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    return clicked;
}

bool Application::RenderWindowControlButton(int icon, const ImVec4& button_color, const ImVec4& hover_color, float width) {
    ImGui::PushID(icon);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(Nord6, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_Button, button_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, hover_color);
    const bool pressed = ImGui::Button("##WindowControl", ImVec2(width, width));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float icon_width = (max.x - min.x) * 0.26f;
    const float icon_height = (max.y - min.y) * 0.26f;
    const float thickness = 2.2f;
    const ImU32 color = ImGui::GetColorU32(Nord6);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    if (icon == WindowControlMinimize) {
        draw_list->AddLine(
            ImVec2(center.x - icon_width, center.y + icon_height * 0.55f),
            ImVec2(center.x + icon_width, center.y + icon_height * 0.55f),
            color,
            thickness
        );
    } else if (icon == WindowControlMaximize) {
        draw_list->AddRect(
            ImVec2(center.x - icon_width, center.y - icon_height),
            ImVec2(center.x + icon_width, center.y + icon_height),
            color,
            0.0f,
            0,
            thickness
        );
    } else if (icon == WindowControlRestore) {
        draw_list->AddRect(
            ImVec2(center.x - icon_width * 0.45f, center.y - icon_height * 0.95f),
            ImVec2(center.x + icon_width * 1.05f, center.y + icon_height * 0.55f),
            color,
            0.0f,
            0,
            thickness
        );
        draw_list->AddRect(
            ImVec2(center.x - icon_width * 1.05f, center.y - icon_height * 0.35f),
            ImVec2(center.x + icon_width * 0.45f, center.y + icon_height * 1.15f),
            color,
            0.0f,
            0,
            thickness
        );
    } else if (icon == WindowControlClose) {
        draw_list->AddLine(
            ImVec2(center.x - icon_width, center.y - icon_height),
            ImVec2(center.x + icon_width, center.y + icon_height),
            color,
            thickness
        );
        draw_list->AddLine(
            ImVec2(center.x + icon_width, center.y - icon_height),
            ImVec2(center.x - icon_width, center.y + icon_height),
            color,
            thickness
        );
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return pressed;
}

void Application::RenderStatusMessage() {
    if (ui_state_.status_message_timer <= 0.0f || ui_state_.status_message.empty()) {
        return;
    }
    ui_state_.status_message_timer -= ImGui::GetIO().DeltaTime;
    ImGui::Spacing();
    BeginPanelCard("StatusMessage");
    RenderSectionHeader("FEEDBACK", "最近操作结果");
    RenderInfoRow("消息", ui_state_.status_message, Nord13);
    EndPanelCard();
}

void Application::BeginPanelCard(const char* id, float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, WithAlpha(Nord1, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(Nord3, 0.38f));
    ImGui::BeginChild(id, ImVec2(0, height), true);
}

void Application::EndPanelCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

} // namespace smile2unlock
