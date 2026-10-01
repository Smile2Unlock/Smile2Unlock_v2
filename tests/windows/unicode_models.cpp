#include "common/utf8_path.h"
#include "platform/windows/paths.h"
#include "runtime/switcher.h"
#include <windows.h>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>

import su.recognizer.backend;
import su.recognizer.image;
import su.recognizer.types;

namespace {
void verify_models(const std::filesystem::path& unicode, const std::filesystem::path& image_file) {
    if (su::recognizer::default_seetaface_model_dir() != unicode) {
        throw std::runtime_error("model environment lost Unicode path");
    }
    const auto paths = su::recognizer::seetaface_model_paths(unicode);
    if (!paths) throw std::runtime_error("staged models missing");
    auto backend = su::recognizer::SeetaFaceBackend(*paths);
    if (!backend.available() || !backend.liveness_available()) {
        throw std::runtime_error("Unicode model directory could not initialize all recognizers");
    }
    const auto image = su::recognizer::load_image_file(image_file);
    if (!image) throw std::runtime_error("Unicode image file could not be decoded");
    const auto result = backend.extract(su::recognizer::ImageView{
        .width = image->width, .height = image->height, .channels = image->channels,
        .bytes = std::span<const std::byte>{image->bytes}}, true);
    if (!result || !result->has_face || result->feature.empty()) {
        throw std::runtime_error("real face extraction from Unicode model directory failed");
    }
    // Modern CPUs can use the baseline DLL without triggering the fallback.
    // Explicitly exercise the same switcher needed by older CPUs, so the
    // Unicode DLL-loading boundary is covered on either kind of host.
    auto switcher = ts::SwitchControll{};
    switcher.auto_switch(ts::ComputingDevice{"cpu", 0});
    if (!switcher.is_load_dll()) throw std::runtime_error("CPU switcher did not load a DLL");
    auto cpu_variant_loaded = false;
    for (const auto* name : {L"libtennis_haswell.dll", L"libtennis_sandy_bridge.dll",
                            L"libtennis_pentium.dll", L"tennis_haswell.dll",
                            L"tennis_sandy_bridge.dll", L"tennis_pentium.dll"}) {
        if (const auto module = ::GetModuleHandleW(name)) {
            auto native = std::wstring(32768, L'\0');
            const auto length = ::GetModuleFileNameW(module, native.data(), static_cast<DWORD>(native.size()));
            if (length == 0 || length >= native.size()) {
                throw std::runtime_error("could not inspect the loaded CPU DLL path");
            }
            native.resize(length);
            if (!std::filesystem::equivalent(std::filesystem::path(native).parent_path(),
                                            su::windows::executable_path().parent_path())) {
                throw std::runtime_error("CPU DLL was loaded outside the Unicode installation directory");
            }
            cpu_variant_loaded = true;
        }
    }
    if (!cpu_variant_loaded) throw std::runtime_error("CPU-specific SDK DLL was not loaded");
    std::cout << "PASS: all five Unicode-path models, image decoding, face extraction and liveness (ACP="
              << ::GetACP() << ", feature=" << result->feature.size() << ")\n";
}
}  // namespace

int main(int argc, char**) try {
    if (argc > 1) {
        const auto models = su::windows::environment_path(L"SU_SEETAFACE_MODEL_DIR");
        const auto image = su::windows::environment_path(L"SU_TEST_MODEL_IMAGE");
        if (!models || !image) throw std::runtime_error("native test environment missing");
        verify_models(*models, *image);
        return 0;
    }
    const auto source = su::windows::executable_path().parent_path() / L"assets" / L"models" / L"seeta";
    const auto fixture = su::windows::executable_path().parent_path() / L"unicode-model-face.png";
    const auto root = su::windows::temporary_directory()
        / (L"su-model-paths-" + std::to_wstring(::GetCurrentProcessId())
            + L"-" + std::to_wstring(::GetTickCount64()));
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    } cleanup{root};
    const auto unicode = root / L"\u4e8e\u5149\u8fdc \u592a\u90ce \u00e9 \U00020000";
    std::filesystem::create_directories(unicode);
    for (const auto* name : {"face_detector.csta", "face_landmarker_pts5.csta",
                             "face_recognizer.csta", "fas_first.csta", "fas_second.csta"}) {
        std::filesystem::copy_file(source / name, unicode / name);
    }
    ::SetEnvironmentVariableW(L"SU_SEETAFACE_MODEL_DIR", unicode.c_str());
    const auto image_file = unicode / L"\u4eba\u8138 \u00e9.png";
    std::filesystem::copy_file(fixture, image_file);
    ::SetEnvironmentVariableW(L"SU_TEST_MODEL_IMAGE", image_file.c_str());

    // Relocate the executable and SDK together: the runtime must locate its
    // CPU-specific TenniS DLL in a Unicode directory, not only read models.
    const auto app_dir = unicode / L"\u7a0b\u5e8f";
    std::filesystem::create_directories(app_dir);
    const auto relocated = app_dir / L"probe.exe";
    std::filesystem::copy_file(su::windows::executable_path(), relocated);
    for (const auto& entry : std::filesystem::directory_iterator(su::windows::executable_path().parent_path())) {
        if (entry.is_regular_file() && entry.path().extension() == L".dll") {
            std::filesystem::copy_file(entry.path(), app_dir / entry.path().filename());
        }
    }
    auto command = L"\"" + relocated.native() + L"\" --exercise";
    auto startup = STARTUPINFOW{};
    startup.cb = sizeof(startup);
    auto process = PROCESS_INFORMATION{};
    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, root.c_str(), &startup, &process)) {
        throw std::runtime_error("could not launch model test in Unicode installation directory");
    }
    const auto wait = ::WaitForSingleObject(process.hProcess, 60000);
    if (wait != WAIT_OBJECT_0) ::TerminateProcess(process.hProcess, 1);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || exit_code != 0) {
        throw std::runtime_error("model test in Unicode installation directory failed");
    }
    std::cout << "PASS: CPU-specific SDK DLL loading from Unicode installation directory\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
