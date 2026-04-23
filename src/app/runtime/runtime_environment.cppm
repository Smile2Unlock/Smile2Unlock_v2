module;

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

export module su.app.runtime;

import std;

export namespace su::app::runtime {

std::filesystem::path executable_dir();
void prepare_runtime_working_directory();

}  // namespace su::app::runtime

namespace su::app::runtime {

std::filesystem::path executable_dir() {
#if defined(_WIN32)
    std::array<char, MAX_PATH> buffer{};
    const auto length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return length == 0 ? std::filesystem::current_path() : std::filesystem::path(std::string_view(buffer.data(), length)).parent_path();
#else
    std::array<char, 4096> buffer{};
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
    return length <= 0 ? std::filesystem::current_path() : std::filesystem::path(std::string_view(buffer.data(), static_cast<std::size_t>(length))).parent_path();
#endif
}

void prepare_runtime_working_directory() {
    std::error_code error;
    const auto target_dir = executable_dir();
    if (!target_dir.empty()) {
        std::filesystem::current_path(target_dir, error);
    }
}

}  // namespace su::app::runtime
