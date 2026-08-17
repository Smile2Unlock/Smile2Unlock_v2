// Windows deployment helper, the counterpart of the Linux su_deploy_helper.
//
// Runs with an elevated token (manifest requireAdministrator) and performs
// the deployment operations the GUI cannot do unprivileged:
//   --register-cp [--dll <path>]   enroll the credential provider
//   --unregister-cp                remove the logon-UI enrollment
//   --ensure-service               start the auth service
//   --inspect                      print the deployment snapshot as JSON
//   --version
//
// The GUI relaunches this executable with ShellExecuteEx(runas) so the UAC
// prompt is the only user interaction; the result is written to
// %TEMP%\su_deploy_result.json (exit code 0 = success).

#include "deployment.h"

#include <windows.h>

#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kResultFile = "su_deploy_result.json";

std::string result_file_path() {
    wchar_t buffer[512] = {};
    const auto length = ::GetTempPathW(512, buffer);
    if (length == 0 || length >= 512) {
        return "su_deploy_result.json";
    }
    return std::string(buffer, buffer + length) + std::string(kResultFile);
}

int write_result(std::string_view content) {
    HANDLE file = ::CreateFileA(
        result_file_path().c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 1;
    }
    DWORD written = 0;
    ::WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    ::CloseHandle(file);
    return 0;
}

int fail(std::string_view detail) {
    const auto json = std::format(R"({{"ok":false,"error":"{}"}})", detail);
    (void)write_result(json);
    std::cerr << "su_deploy_helper: " << detail << "\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dll_path = "C:\\su-deploy\\su_credential_provider.dll";
    std::string_view operation;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--dll" && i + 1 < argc) {
            dll_path = argv[++i];
        } else if (argument == "--version") {
            std::cout << "su_deploy_helper 1\n";
            return 0;
        } else {
            operation = argument;
        }
    }

    if (operation == "--inspect") {
        const auto snapshot = su::windeploy::inspect_deployment();
        if (!snapshot) {
            return fail(snapshot.error());
        }
        const auto payload = std::format(
            R"({{"ok":true,"snapshot":{}}})", su::windeploy::snapshot_json(*snapshot));
        std::cout << payload << "\n";
        return write_result(payload);
    }
    if (operation == "--register-cp") {
        const auto registered = su::windeploy::register_credential_provider(dll_path);
        if (!registered) {
            return fail(registered.error());
        }
        std::cout << "credential provider registered\n";
        return write_result(R"({"ok":true,"action":"register-cp"})");
    }
    if (operation == "--unregister-cp") {
        const auto unregistered = su::windeploy::unregister_credential_provider();
        if (!unregistered) {
            return fail(unregistered.error());
        }
        std::cout << "credential provider unregistered\n";
        return write_result(R"({"ok":true,"action":"unregister-cp"})");
    }
    if (operation == "--ensure-service") {
        const auto ensured = su::windeploy::ensure_auth_service();
        if (!ensured) {
            return fail(ensured.error());
        }
        std::cout << "auth service ensured\n";
        return write_result(R"({"ok":true,"action":"ensure-service"})");
    }

    std::cerr << "usage: su_deploy_helper [--dll <path>] "
                 "(--register-cp | --unregister-cp | --ensure-service | --inspect | --version)\n";
    return 64;
}
