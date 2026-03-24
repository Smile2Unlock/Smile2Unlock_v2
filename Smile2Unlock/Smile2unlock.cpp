#include "registryhelper.h"

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

    smile2unlock::BackendService backend;
    if (!backend.Initialize()) {
        std::cerr << "Failed to initialize Smile2Unlock service backend!" << std::endl;
        return -1;
    }

    smile2unlock::ServiceRuntime runtime;
    runtime.Run();
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

    smile2unlock::Application app;
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
    return WinMain(GetModuleHandle(nullptr), nullptr, GetCommandLineA(), SW_SHOW);
}
