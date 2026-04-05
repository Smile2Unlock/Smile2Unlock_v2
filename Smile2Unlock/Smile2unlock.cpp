#include "registryhelper.h"
#include "backend/managers/ipc/gui_ipc_server.h"
#include "backend/gui_ipc_handler.h"
#include "backend/remote_backend_service.h"

#include <shellapi.h>
#include <windows.h>

import service_runtime;
import smile2unlock.application;
import smile2unlock.service;
import std;

namespace {

bool HasArg(const std::vector<std::string>& args, const std::string& value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}

void RegisterInstallPath() {
    char exe_path[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exe_path, MAX_PATH)) {
        return;
    }

    const auto install_dir = std::filesystem::path(exe_path).parent_path().string();
    RegistryHelper::WriteStringToRegistry(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\path",
        install_dir
    );
}

int RunServiceMode() {
    RegisterInstallPath();

    // 创建服务互斥体，确保只有一个 Service 进程
    smile2unlock::ServiceMutex service_mutex;
    if (!service_mutex.try_create()) {
        std::cerr << "Another Service instance is already running!" << std::endl;
        return -1;
    }

    // 初始化后端服务
    auto backend = std::make_unique<smile2unlock::BackendService>();
    if (!backend->Initialize()) {
        std::cerr << "Failed to initialize Smile2Unlock service backend!" << std::endl;
        return -1;
    }

    // 创建 IPC 请求处理器
    smile2unlock::GuiIpcRequestHandler ipc_handler(backend.get());

    // 创建并启动 IPC 服务端
    smile2unlock::GuiIpcServer ipc_server;
    ipc_server.set_handler([&ipc_handler](const smile2unlock::GuiIpcRequest& req, smile2unlock::GuiIpcResponse& resp) {
        ipc_handler.handle(req, resp);
    });
    ipc_server.start();

    std::cout << "Smile2Unlock Service started, waiting for GUI connections..." << std::endl;

    // 运行 ServiceRuntime（带超时检测）
    smile2unlock::ServiceRuntime runtime;
    runtime.Run();

    // 清理
    ipc_server.stop();
    service_mutex.release();

    return 0;
}

int RunGuiMode() {
    RegisterInstallPath();

    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
    }

    std::cout << "Smile2Unlock GUI Starting..." << std::endl;

    // 检测是否有 Service 进程在运行
    bool service_running = smile2unlock::ServiceMutex::is_service_running();
    
    smile2unlock::Application app;
    
    if (service_running) {
        // 使用远程后端连接到已运行的 Service
        std::cout << "Detected running Service, connecting via IPC..." << std::endl;
        
        auto remote_backend = std::make_unique<smile2unlock::RemoteBackendService>();
        if (!remote_backend->Initialize()) {
            std::cerr << "Failed to connect to Service! Falling back to local mode." << std::endl;
            // 降级到本地模式
            auto local_backend = std::make_unique<smile2unlock::BackendService>();
            if (!local_backend->Initialize()) {
                std::cerr << "Failed to initialize local backend!" << std::endl;
                return -1;
            }
            app.SetBackend(std::move(local_backend));
            app.SetRemoteBackendFlag(false);
        } else {
            app.SetBackend(std::move(remote_backend));
            app.SetRemoteBackendFlag(true);
            std::cout << "Connected to Service via IPC." << std::endl;
        }
    } else {
        // 使用本地后端（内置 Service 功能，无超时）
        std::cout << "No Service detected, running in standalone mode." << std::endl;
        
        auto local_backend = std::make_unique<smile2unlock::BackendService>();
        if (!local_backend->Initialize()) {
            std::cerr << "Failed to initialize backend!" << std::endl;
            return -1;
        }
        app.SetBackend(std::move(local_backend));
        app.SetRemoteBackendFlag(false);
    }

    if (!app.Initialize()) {
        std::cerr << "Failed to initialize application!" << std::endl;
        return -1;
    }

    app.Run();
    return 0;
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    int argc = 0;
    LPWSTR* argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;

    if (argv_w != nullptr) {
        for (int i = 1; i < argc; ++i) {
            const int required = WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, nullptr, 0, nullptr, nullptr);
            if (required > 1) {
                std::string arg(static_cast<size_t>(required), '\0');
                WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, arg.data(), required, nullptr, nullptr);
                arg.resize(static_cast<size_t>(required - 1));
                args.push_back(std::move(arg));
            }
        }
        LocalFree(argv_w);
    }

    if (HasArg(args, "--service")) {
        return RunServiceMode();
    }

    return RunGuiMode();
}

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(65001);
#endif
  return WinMain(GetModuleHandle(nullptr), nullptr, GetCommandLineA(), SW_SHOW);
}
