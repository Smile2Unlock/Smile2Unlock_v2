// Exercises the real helper without registering a CP or installing a service.
// Run under Wine, or from an elevated Windows test process (helper manifest).
#include "result_file.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
    return condition;
}

DWORD run_helper(const std::wstring& helper, const wchar_t* argument) {
    auto command = L"\"" + helper + L"\" " + argument;
    auto startup = STARTUPINFOW{};
    startup.cb = sizeof(startup);
    auto process = PROCESS_INFORMATION{};
    if (!check(::CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr,
                               FALSE, 0, nullptr, nullptr, &startup, &process) != 0,
               "launch real deployment helper")) {
        return DWORD(-1);
    }
    ::CloseHandle(process.hThread);
    const auto waited = ::WaitForSingleObject(process.hProcess, 60000);
    auto code = DWORD(-1);
    if (check(waited == WAIT_OBJECT_0, "helper completes before timeout")) {
        check(::GetExitCodeProcess(process.hProcess, &code) != 0, "read helper exit code");
    } else {
        ::TerminateProcess(process.hProcess, 1);
        ::WaitForSingleObject(process.hProcess, 5000);
    }
    ::CloseHandle(process.hProcess);
    return code;
}

nlohmann::json read_result(const std::wstring& path) {
    const auto file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!check(file != INVALID_HANDLE_VALUE, "read result using UTF-16 path")) {
        return nullptr;
    }
    char bytes[8192] = {};
    DWORD count = 0;
    const auto read = ::ReadFile(file, bytes, sizeof(bytes), &count, nullptr);
    ::CloseHandle(file);
    if (!check(read != 0, "read helper JSON bytes")) {
        return nullptr;
    }
    return nlohmann::json::parse(bytes, bytes + count, nullptr, false);
}

void writable_temp(const std::wstring& helper, const std::wstring& directory, bool signed_package) {
    std::filesystem::create_directories(directory);
    check(::SetEnvironmentVariableW(L"TEMP", directory.c_str()) != 0, "set TEMP");
    check(::SetEnvironmentVariableW(L"TMP", directory.c_str()) != 0, "set TMP");
    const auto result = directory + L"\\su_deploy_result.json";
    const auto resolved = su::windeploy::result_file_path();
    check(resolved && *resolved == result, "GUI and helper resolve the exact UTF-16 result path");

    check(run_helper(helper, L"--inspect") == 0, "successful inspect exits zero");
    const auto snapshot = read_result(result);
    check(snapshot.is_object() && snapshot.value("ok", false) && snapshot.contains("snapshot"),
          "successful inspect writes its snapshot");
    check(::DeleteFileW(result.c_str()) != 0, "delete previous result using UTF-16 path");

    check(run_helper(helper, L"--unknown") == 1, "invalid operation exits one");
    const auto failure = read_result(result);
    check(failure.is_object() && !failure.value("ok", true) && failure.contains("error"),
          "failed operation writes its error JSON");
    check(::DeleteFileW(result.c_str()) != 0, "delete failure result");

    // The default test uses the build tree, which is deliberately not a signed
    // package. Release CI also passes the actual freshly signed package here.
    check(run_helper(helper, L"--verify") == (signed_package ? 0 : 1),
          "verify exit code reflects package validity");
    const auto verification = read_result(result);
    check(verification.is_object() && verification.value("ok", !signed_package) == signed_package,
          "verify result reflects package validity");
    check(::DeleteFileW(result.c_str()) != 0, "delete verification result");

    // A directory at the result filename forces CreateFileW to fail. The
    // actual action's exit code must survive both success and failure cases.
    check(::CreateDirectoryW(result.c_str(), nullptr) != 0, "block result file creation");
    check(run_helper(helper, L"--inspect") == 0, "JSON I/O failure does not fail successful inspect");
    check(run_helper(helper, L"--unknown") == 1, "JSON I/O failure does not mask operation failure");
    check(run_helper(helper, L"--verify") == (signed_package ? 0 : 1),
          "JSON I/O failure does not change verification exit code");
    check(::RemoveDirectoryW(result.c_str()) != 0, "remove result blocker");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: deploy_helper_result_test <helper.exe> [--signed-package]\n";
        return 2;
    }
    const auto signed_package = argc == 3 && std::wstring_view(argv[2]) == L"--signed-package";
    const auto initial = su::windeploy::result_file_path();
    if (!check(initial.has_value(), "resolve initial temporary directory")) {
        return 1;
    }
    const auto root = std::filesystem::path(*initial).parent_path()
        / (L"s2u-result-test-" + std::to_wstring(::GetCurrentProcessId()));
    const auto helper = std::filesystem::absolute(argv[1]).wstring();
    writable_temp(helper, (root / L"ascii-temp").wstring(), signed_package);
    writable_temp(helper, (root / L"\u4e8e\u5149\u8fdc \u4e34\u65f6\u76ee\u5f55").wstring(), signed_package);
    std::filesystem::remove_all(root);
    if (failures == 0) {
        std::cout << "PASS: ASCII/Unicode TEMP, result JSON, and operation exit codes\n";
    }
    return failures == 0 ? 0 : 1;
}
