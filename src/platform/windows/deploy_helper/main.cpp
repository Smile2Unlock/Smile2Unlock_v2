// Windows deployment helper, the counterpart of the Linux su_deploy_helper.
//
// Runs with an elevated token (manifest requireAdministrator) and performs
// the deployment operations the GUI cannot do unprivileged:
//   --register-cp                  enroll the packaged credential provider
//   --unregister-cp                remove the logon-UI enrollment
//   --ensure-service               start the auth service
//   --inspect                      print the deployment snapshot as JSON
//   --verify                       validate the signed package beside this exe
//   --version
//
// The GUI relaunches this executable with ShellExecuteEx(runas) so the UAC
// prompt is the only user interaction; the result is written to
// %TEMP%\su_deploy_result.json (exit code 0 = success).

#include "deployment.h"
#include "result_file.h"

#include <windows.h>

#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void write_result(std::string_view content) {
    const auto path = su::windeploy::result_file_path();
    if (!path) {
        std::cerr << "su_deploy_helper: failed to resolve result path (Win32 error "
                  << path.error() << ")\n";
        return;
    }
    HANDLE file = ::CreateFileW(
        path->c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::cerr << "su_deploy_helper: failed to create result file (Win32 error "
                  << ::GetLastError() << ")\n";
        return;
    }
    DWORD written = 0;
    const auto succeeded = ::WriteFile(
        file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    const auto error = succeeded ? ERROR_WRITE_FAULT : ::GetLastError();
    ::CloseHandle(file);
    if (!succeeded || written != content.size()) {
        std::cerr << "su_deploy_helper: failed to write complete result file (Win32 error "
                  << error << ")\n";
    }
}

int succeed(std::string_view content) {
    // The installer consumes the operation's exit code; the GUI consumes the
    // JSON. Report I/O failures separately without undoing a successful action.
    write_result(content);
    return 0;
}

int fail(std::string_view detail) {
    auto escaped = std::string{};
    escaped.reserve(detail.size());
    for (const auto ch : detail) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                escaped += std::format("\\u{:04x}", static_cast<unsigned char>(ch));
            } else {
                escaped += ch;
            }
            break;
        }
    }
    const auto json = std::format(R"({{"ok":false,"error":"{}"}})", escaped);
    write_result(json);
    std::cerr << "su_deploy_helper: " << detail << "\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    auto register_cp = false;
    auto unregister_cp = false;
    auto ensure_service = false;
    auto inspect = false;
    auto verify = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--version") {
            std::cout << "su_deploy_helper " << SU_VERSION_STR << "\n";
            return 0;
        } else if (argument == "--register-cp") {
            register_cp = true;
        } else if (argument == "--unregister-cp") {
            unregister_cp = true;
        } else if (argument == "--ensure-service") {
            ensure_service = true;
        } else if (argument == "--inspect") {
            inspect = true;
        } else if (argument == "--verify") {
            verify = true;
        } else {
            return fail(std::format("unknown argument: {}", argument));
        }
    }

    if (inspect) {
        if (register_cp || unregister_cp || ensure_service) {
            return fail("--inspect cannot be combined with deployment operations");
        }
        const auto snapshot = su::windeploy::inspect_deployment();
        if (!snapshot) {
            return fail(snapshot.error());
        }
        const auto payload = std::format(
            R"({{"ok":true,"snapshot":{}}})", su::windeploy::snapshot_json(*snapshot));
        std::cout << payload << "\n";
        return succeed(payload);
    }
    if (verify) {
        if (register_cp || unregister_cp || ensure_service || inspect) {
            return fail("--verify cannot be combined with deployment operations");
        }
        const auto validated = su::windeploy::validate_deployment_package();
        if (!validated) {
            return fail(validated.error());
        }
        std::cout << "deployment package verified\n";
        return succeed(R"({"ok":true,"action":"verify"})");
    }
    if (register_cp && unregister_cp) {
        return fail("--register-cp and --unregister-cp cannot be combined");
    }
    if (register_cp) {
        const auto registered = su::windeploy::register_credential_provider();
        if (!registered) {
            return fail(registered.error());
        }
        std::cout << "credential provider registered\n";
    }
    if (unregister_cp) {
        const auto unregistered = su::windeploy::unregister_credential_provider();
        if (!unregistered) {
            return fail(unregistered.error());
        }
        std::cout << "credential provider unregistered\n";
    }
    if (ensure_service) {
        const auto ensured = su::windeploy::ensure_auth_service();
        if (!ensured) {
            return fail(ensured.error());
        }
        std::cout << "auth service ensured\n";
    }
    if (register_cp || unregister_cp || ensure_service) {
        return succeed(R"({"ok":true,"action":"deploy"})");
    }

    std::cerr << "usage: su_deploy_helper "
                 "(--register-cp | --unregister-cp | --ensure-service | --inspect | --verify | --version)\n";
    return 64;
}
