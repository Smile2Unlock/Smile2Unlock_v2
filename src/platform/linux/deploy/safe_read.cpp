#include "platform/linux/deploy/safe_read.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace su::deploy {
namespace {
constexpr std::size_t kReadChunkBytes = 64U * 1024U;
} // namespace

OpenedFile::OpenedFile(OpenedFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), metadata_(other.metadata_) {}

OpenedFile& OpenedFile::operator=(OpenedFile&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = std::exchange(other.fd_, -1);
        metadata_ = other.metadata_;
    }
    return *this;
}

OpenedFile::~OpenedFile() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

std::expected<OpenedFile, OpenFailure> open_regular_nofollow(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        switch (errno) {
        case ENOENT:
        case ENOTDIR:
            return std::unexpected(OpenFailure::kMissing);
        case ELOOP:
            return std::unexpected(OpenFailure::kSymlink);
        default:
            return std::unexpected(OpenFailure::kUnreadable);
        }
    }
    struct stat metadata {};
    if (::fstat(fd, &metadata) != 0) {
        ::close(fd);
        return std::unexpected(OpenFailure::kUnreadable);
    }
    if (!S_ISREG(metadata.st_mode)) {
        ::close(fd);
        return std::unexpected(OpenFailure::kNotRegular);
    }
    return OpenedFile{fd, metadata};
}

std::expected<std::string, ReadFailure> read_capped(const OpenedFile& file, std::size_t cap) {
    // Compare in the unsigned domain; st_size of a regular file is never negative.
    if (std::cmp_greater(file.metadata().st_size, cap)) {
        return std::unexpected(ReadFailure::kTooLarge);
    }
    auto content = std::string{};
    auto chunk = std::array<char, kReadChunkBytes>{};
    while (content.size() <= cap) {
        const auto want = std::min(chunk.size(), cap + 1 - content.size());
        const auto got = ::read(file.fd(), chunk.data(), want);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(ReadFailure::kIo);
        }
        if (got == 0) {
            return content;
        }
        content.append(chunk.data(), static_cast<std::size_t>(got));
    }
    return std::unexpected(ReadFailure::kTooLarge);
}

} // namespace su::deploy
