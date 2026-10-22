#include "windows_directory.h"
#include <mutex>

namespace su::log {
namespace {
struct FileState {
    std::filesystem::path path;
    RotationPolicy policy;
    bool system;
    std::mutex mutex;
    Handle handle;
};
std::expected<Handle, SinkError> open_file(const std::filesystem::path& path, bool system,
                                           DWORD disposition) {
    auto handle = Handle{
        CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL,
                    FILE_SHARE_READ, nullptr, disposition, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (handle.get() == INVALID_HANDLE_VALUE)
        return std::unexpected(SinkError::open_failed);
    if (const auto checked = check_handle(handle.get(), false, system); !checked)
        return std::unexpected(checked.error());
    return handle;
}
std::filesystem::path rotation_path(const std::filesystem::path& base, unsigned index) {
    auto path = base;
    path += L"." + std::to_wstring(index);
    return path;
}
std::expected<void, SinkError> check_optional_file(const std::filesystem::path& path, bool system) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
        return {};
    const auto handle = open_file(path, system, OPEN_EXISTING);
    return handle ? std::expected<void, SinkError>{} : std::unexpected(handle.error());
}
std::expected<void, SinkError> rotate(FileState& state) {
    state.handle.reset();
    if (!state.policy.keep) {
        return DeleteFileW(state.path.c_str()) ? std::expected<void, SinkError>{}
                                               : std::unexpected(SinkError::rotate_failed);
    }
    for (auto index = state.policy.keep; index > 0; --index) {
        const auto source = index == 1 ? state.path : rotation_path(state.path, index - 1);
        const auto destination = rotation_path(state.path, index);
        if (!check_optional_file(source, state.system) ||
            !check_optional_file(destination, state.system))
            return std::unexpected(SinkError::rotate_failed);
        if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING) &&
            GetLastError() != ERROR_FILE_NOT_FOUND)
            return std::unexpected(SinkError::rotate_failed);
    }
    return {};
}
std::expected<void, SinkError> append(FileState& state, std::string_view line) {
    auto parents = check_directory_chain(state.path.parent_path(), state.system);
    if (!parents)
        return std::unexpected(parents.error());
    if (!state.handle) {
        auto opened = open_file(state.path, state.system, OPEN_ALWAYS);
        if (!opened)
            return std::unexpected(opened.error());
        state.handle = std::move(*opened);
    }
    auto size = LARGE_INTEGER{};
    if (!GetFileSizeEx(state.handle.get(), &size))
        return std::unexpected(SinkError::write_failed);
    if (std::holds_alternative<Rotate>(plan_write(size.QuadPart, line.size(), state.policy))) {
        if (const auto rotated = rotate(state); !rotated)
            return rotated;
        auto opened = open_file(state.path, state.system, OPEN_ALWAYS);
        if (!opened)
            return std::unexpected(opened.error());
        state.handle = std::move(*opened);
    }
    auto offset = std::size_t{0};
    while (offset < line.size()) {
        DWORD written = 0;
        if (!WriteFile(state.handle.get(), line.data() + offset,
                       static_cast<DWORD>(line.size() - offset), &written, nullptr) ||
            !written)
            return std::unexpected(SinkError::write_failed);
        offset += written;
    }
    return {};
}
} // namespace
Sink rotating_file_sink(std::filesystem::path path, RotationPolicy policy, bool system) {
    const auto state = std::make_shared<FileState>();
    state->path = std::move(path);
    state->policy = policy;
    state->system = system;
    return [state](Level, std::string_view line) -> std::expected<void, SinkError> {
        const auto lock = std::lock_guard{state->mutex};
        try {
            return append(*state, line);
        } catch (...) {
            return std::unexpected(SinkError::write_failed);
        }
    };
}
Sink debugger_sink() {
    return [](Level, std::string_view line) -> std::expected<void, SinkError> {
        const auto count =
            MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
        auto wide = std::wstring(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), wide.data(),
                            count);
        OutputDebugStringW(wide.c_str());
        return {};
    };
}
std::vector<Field> win32_error_fields(unsigned long error) {
    wchar_t* raw = nullptr;
    const auto count = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    const auto memory = std::unique_ptr<wchar_t, decltype(&LocalFree)>{raw, &LocalFree};
    const auto bytes =
        count ? WideCharToMultiByte(CP_UTF8, 0, raw, count, nullptr, 0, nullptr, nullptr) : 0;
    auto text = std::string(static_cast<std::size_t>(bytes), '\0');
    if (bytes)
        WideCharToMultiByte(CP_UTF8, 0, raw, count, text.data(), bytes, nullptr, nullptr);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
        text.pop_back();
    return {field("win32", error), field("win32_text", text)};
}
} // namespace su::log
