#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>

#include <sys/stat.h>

namespace su::deploy {

// Owns a file descriptor; the fstat() snapshot is taken from the same fd that
// is later read, so the checks cannot be raced by swapping the path.
class OpenedFile {
public:
    OpenedFile(int fd, const struct stat& metadata) noexcept : fd_(fd), metadata_(metadata) {}
    OpenedFile(OpenedFile&& other) noexcept;
    OpenedFile& operator=(OpenedFile&& other) noexcept;
    OpenedFile(const OpenedFile&) = delete;
    OpenedFile& operator=(const OpenedFile&) = delete;
    ~OpenedFile();

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] const struct stat& metadata() const noexcept { return metadata_; }

private:
    int fd_ = -1;
    struct stat metadata_ {};
};

enum class OpenFailure {
    kMissing,   // ENOENT / ENOTDIR: nothing lives at the path.
    kSymlink,   // The final component is a symbolic link (ELOOP under O_NOFOLLOW).
    kNotRegular, // FIFO, device, directory, ...
    kUnreadable,
};

enum class ReadFailure {
    kTooLarge,
    kIo,
};

// O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC, then fstat(): O_NONBLOCK keeps a
// planted FIFO from hanging the caller, and S_ISREG rejects it right after.
[[nodiscard]] std::expected<OpenedFile, OpenFailure> open_regular_nofollow(
    const std::filesystem::path& path);

// Reads at most `cap + 1` bytes so growth after fstat() is still detected.
[[nodiscard]] std::expected<std::string, ReadFailure> read_capped(
    const OpenedFile& file, std::size_t cap);

} // namespace su::deploy
