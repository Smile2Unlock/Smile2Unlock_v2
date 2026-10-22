#include "sinks.h"
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace su::log {
namespace {
struct FileState {
    std::filesystem::path path;
    RotationPolicy policy;
    std::mutex mutex;
    int handle = -1;
    ~FileState() {
        if (handle >= 0)
            close(handle);
    }
};
std::expected<void, SinkError> rotate(FileState& state) {
    if (state.handle >= 0)
        close(std::exchange(state.handle, -1));
    auto error = std::error_code{};
    if (!state.policy.keep) {
        std::filesystem::remove(state.path, error);
    } else {
        for (auto index = state.policy.keep; index > 0; --index) {
            const auto source =
                index == 1 ? state.path
                           : std::filesystem::path{rotated_name(state.path.string(), index - 1)};
            const auto destination = rotated_name(state.path.string(), index);
            if (::rename(source.c_str(), destination.c_str()) != 0 && errno != ENOENT)
                return std::unexpected(SinkError::rotate_failed);
        }
    }
    return error ? std::expected<void, SinkError>{std::unexpected(SinkError::rotate_failed)}
                 : std::expected<void, SinkError>{};
}
std::expected<void, SinkError> open_file(FileState& state) {
    if (state.handle < 0)
        state.handle =
            open(state.path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat attributes{};
    if (state.handle < 0)
        return std::unexpected(errno == ELOOP ? SinkError::reparse_point : SinkError::open_failed);
    if (fstat(state.handle, &attributes) || !S_ISREG(attributes.st_mode))
        return std::unexpected(SinkError::open_failed);
    return {};
}
std::expected<void, SinkError> append(FileState& state, std::string_view line) {
    if (const auto opened = open_file(state); !opened)
        return opened;
    struct stat attributes{};
    if (fstat(state.handle, &attributes))
        return std::unexpected(SinkError::write_failed);
    if (std::holds_alternative<Rotate>(plan_write(attributes.st_size, line.size(), state.policy))) {
        if (const auto rotated = rotate(state); !rotated)
            return rotated;
        if (const auto opened = open_file(state); !opened)
            return opened;
    }
    for (auto offset = std::size_t{0}; offset < line.size();) {
        const auto count = write(state.handle, line.data() + offset, line.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return std::unexpected(SinkError::write_failed);
        offset += count;
    }
    return {};
}
} // namespace
Sink rotating_file_sink(std::filesystem::path path, RotationPolicy policy, bool) {
    const auto state = std::make_shared<FileState>();
    state->path = std::move(path);
    state->policy = policy;
    return [state](Level, std::string_view line) -> std::expected<void, SinkError> {
        const auto lock = std::lock_guard{state->mutex};
        try {
            return append(*state, line);
        } catch (...) {
            return std::unexpected(SinkError::write_failed);
        }
    };
}
} // namespace su::log
