#include "common/utf8_path.h"
#include "platform/windows/paths.h"
#include "su_core.h"

#include <windows.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

import su.app.preferences;
import su.app.i18n;

namespace {
int failures = 0;
void check(bool value, const char* description) {
    if (!value) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}
}  // namespace

int main(int argc, char**) try {
    if (argc > 1) {
        const auto expected = su::windows::environment_path(L"SU_TEST_EXPECTED_EXE");
        check(expected && su::windows::executable_path() == *expected,
              "Unicode executable path survives relocation and unrelated working directory");
        return failures == 0 ? 0 : 1;
    }

    const auto root = su::windows::temporary_directory()
        / (L"su-unicode-paths-" + std::to_wstring(::GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const auto names = {L"alice", L"\u4e8e\u5149\u8fdc space", L"\u00e9mile",
                        L"\u592a\u90ce", L"\U00020000user"};
    const auto original_temp = su::windows::environment_path(L"TEMP");
    const auto original_tmp = su::windows::environment_path(L"TMP");
    for (const auto* name : names) {
        const auto profile = root / name;
        const auto roaming = profile / L"AppData" / L"Roaming";
        // Poison the CRT copy first. The native environment is authoritative
        // even if getenv retains stale or non-UTF-8 bytes.
        ::_putenv("APPDATA=C:\\stale-ansi-copy");
        check(::SetEnvironmentVariableW(L"APPDATA", roaming.c_str()), "set native APPDATA");
        check(su::windows::roaming_app_data() == roaming, "APPDATA preserves native Unicode path");
        check(::SetEnvironmentVariableW(L"APPDATA", nullptr), "clear APPDATA for fallback");
        check(::SetEnvironmentVariableW(L"USERPROFILE", profile.c_str()), "set native USERPROFILE");
        check(su::windows::roaming_app_data() == roaming, "USERPROFILE fallback preserves Unicode");

        const auto config_path = roaming / L"smile2unlock" / L"config.toml";
        const auto utf8 = su::path_utf8(config_path);
        check(su::path_from_utf8(utf8) == config_path, "native path round-trips through explicit UTF-8");
        auto config = su_core_default_config();
        config.selected_camera = 7;
        check(su_core_save_config(utf8.c_str(), &config) == SuStatus_Ok,
              "Rust saves config through UTF-8 FFI on Unicode directory");
        auto loaded = SuCoreConfig{};
        check(su_core_load_config(utf8.c_str(), &loaded) == SuStatus_Ok
              && loaded.selected_camera == 7, "Rust loads saved config from exact same path");
        check(std::filesystem::is_regular_file(config_path), "config is at the intended native path");
        const auto preference = roaming / L"smile2unlock" / L"ui.json";
        std::ofstream(preference) << "{";
        const auto invalid_ui = su::app::load_ui_preferences(preference);
        check(!invalid_ui && invalid_ui.error().find(su::path_utf8(preference)) != std::string::npos,
              "invalid UI JSON reports the Unicode path as UTF-8");
        auto preferences = su::app::UiPreferences{};
        preferences.language = "zh-CN";
        preferences.theme = su::app::ThemePreference::dark;
        check(su::app::save_ui_preferences(preference, preferences).has_value(),
              "UI preferences save atomically to Unicode profile directory");
        check(su::app::load_ui_preferences(preference) == preferences,
              "UI preferences reload from exact same Unicode path");
        const auto languages = profile / L"languages";
        std::filesystem::create_directories(languages);
        const auto pack = languages / L"\u4e2d\u6587.json";
        std::ofstream(pack) << "{";
        const auto invalid_pack = su::app::LanguageCatalog::load(languages);
        check(!invalid_pack && invalid_pack.error().find(su::path_utf8(languages)) != std::string::npos,
              "invalid language JSON reports the Unicode path as UTF-8");
        const auto temp = profile / L"\u4e34\u65f6\u76ee\u5f55";
        std::filesystem::create_directories(temp);
        ::SetEnvironmentVariableW(L"TEMP", temp.c_str());
        ::SetEnvironmentVariableW(L"TMP", temp.c_str());
        check(su::windows::temporary_directory().lexically_normal() == (temp / L"").lexically_normal(),
              "native TEMP preserves Unicode independently of the CRT environment copy");
    }
    ::SetEnvironmentVariableW(L"TEMP", original_temp ? original_temp->c_str() : nullptr);
    ::SetEnvironmentVariableW(L"TMP", original_tmp ? original_tmp->c_str() : nullptr);

    const auto long_value = std::wstring(4000, L'\u4e8e');
    check(::SetEnvironmentVariableW(L"SU_TEST_LONG_PATH", long_value.c_str()), "set long native environment value");
    const auto long_path = su::windows::environment_path(L"SU_TEST_LONG_PATH");
    check(long_path && long_path->native() == long_value, "environment reader grows without truncation");
    ::SetEnvironmentVariableW(L"SU_TEST_LONG_PATH", L"");
    check(!su::windows::environment_path(L"SU_TEST_LONG_PATH"), "empty environment is absent");
    ::SetEnvironmentVariableW(L"SU_TEST_LONG_PATH", nullptr);
    check(!su::windows::environment_path(L"SU_TEST_LONG_PATH"), "missing environment is absent");

    const auto relocated = root / L"\u4e2d\u6587 \u592a\u90ce \U00020000" / L"probe.exe";
    std::filesystem::create_directories(relocated.parent_path());
    std::filesystem::copy_file(su::windows::executable_path(), relocated);
    ::SetEnvironmentVariableW(L"SU_TEST_EXPECTED_EXE", relocated.c_str());
    auto command = L"\"" + relocated.native() + L"\" --relocated";
    auto startup = STARTUPINFOW{};
    startup.cb = sizeof(startup);
    auto process = PROCESS_INFORMATION{};
    const auto launched = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr,
        FALSE, 0, nullptr, root.c_str(), &startup, &process);
    check(launched, "launch relocated executable through CreateProcessW");
    if (launched) {
        const auto wait = ::WaitForSingleObject(process.hProcess, 30000);
        if (wait != WAIT_OBJECT_0) ::TerminateProcess(process.hProcess, 1);
        DWORD exit_code = 1;
        ::GetExitCodeProcess(process.hProcess, &exit_code);
        check(wait == WAIT_OBJECT_0 && exit_code == 0, "relocated process resolves exact executable directory");
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
    }
    std::filesystem::remove_all(root);
    if (failures == 0) std::cout << "PASS: native environment, Unicode relocation, and Rust config I/O (ACP=" << ::GetACP() << ")\n";
    return failures == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
