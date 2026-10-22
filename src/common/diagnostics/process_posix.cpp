#include "process.h"
#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
namespace su::diagnostics {
namespace {
std::expected<std::string, Error> drain(int fd, pid_t child, std::size_t cap,
                                        std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto output = std::string{};
    auto finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        auto descriptor = pollfd{fd, POLLIN, 0};
        if (poll(&descriptor, 1, 100) < 0 && errno != EINTR)
            break;
        auto buffer = std::array<char, 8192>{};
        const auto count = read(fd, buffer.data(), buffer.size());
        if (count == 0) {
            finished = true;
            break;
        }
        if (count < 0 && errno != EAGAIN && errno != EINTR)
            break;
        if (count > 0 && output.size() <= cap)
            output.append(buffer.data(),
                          std::min(static_cast<std::size_t>(count), cap + 1 - output.size()));
    }
    close(fd);
    if (!finished)
        kill(child, SIGKILL);
    auto status = int{};
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return std::unexpected(Error{ErrorCode::process, "waitpid failed"});
    if (!finished)
        return std::unexpected(
            Error{ErrorCode::process, "command timed out or output read failed"});
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return std::unexpected(
            Error{ErrorCode::process, "command failed: " + truncate_output(output, 4096)});
    return truncate_output(output, cap);
}
} // namespace
std::expected<std::string, Error> run_command(const std::vector<std::string>& args, std::size_t cap,
                                              std::chrono::seconds timeout) {
    if (args.empty() || !args.front().starts_with('/'))
        return std::unexpected(Error{ErrorCode::process, "executable must be absolute"});
    auto argv = std::vector<char*>{};
    for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC) != 0)
        return std::unexpected(Error{ErrorCode::process, "pipe failed"});
    const auto child = fork();
    if (child == 0) {
        dup2(descriptors[1], STDOUT_FILENO);
        dup2(descriptors[1], STDERR_FILENO);
        close(descriptors[0]);
        close(descriptors[1]);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(descriptors[1]);
    if (child < 0) {
        close(descriptors[0]);
        return std::unexpected(Error{ErrorCode::process, "fork failed"});
    }
    fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    return drain(descriptors[0], child, cap, timeout);
}
} // namespace su::diagnostics
