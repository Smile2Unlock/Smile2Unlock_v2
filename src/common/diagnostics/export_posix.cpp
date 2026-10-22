#include "export.h"
#include "process.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
namespace su::diagnostics {
namespace {
std::string unquote(std::string text) {
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') return text.substr(1, text.size() - 2);
    return text;
}
}
std::expected<std::filesystem::path, Error> desktop_directory() {
    const auto* raw = std::getenv("HOME");
    if (!raw || !*raw) return std::unexpected(Error{ErrorCode::io, "HOME is unavailable"});
    const auto home = std::filesystem::path{raw};
    const auto* config = std::getenv("XDG_CONFIG_HOME");
    const auto config_path = config && std::filesystem::path{config}.is_absolute()
        ? std::filesystem::path{config} : home / ".config";
    auto dirs = std::ifstream(config_path / "user-dirs.dirs");
    std::string line;
    while (std::getline(dirs, line)) {
        if (!line.starts_with("XDG_DESKTOP_DIR=")) continue;
        auto value = unquote(line.substr(16));
        if (value.starts_with("$HOME")) value.replace(0, 5, home.string());
        const auto path = std::filesystem::path{value};
        if (path.is_absolute() && std::filesystem::is_directory(path)) return path;
    }
    if (std::filesystem::is_directory(home / "Desktop")) return home / "Desktop";
    return home;
}
std::expected<std::string, Error> operating_system() {
    auto input = std::ifstream("/etc/os-release");
    std::string line, pretty;
    while (std::getline(input, line)) if (line.starts_with("PRETTY_NAME=")) pretty = unquote(line.substr(12));
    auto info = utsname{};
    if (uname(&info) < 0) return std::unexpected(Error{ErrorCode::io, "uname failed"});
    return pretty + "\n" + info.sysname + " " + info.release + " " + info.machine;
}
std::expected<bool, Error> reserve_archive(const std::filesystem::path& path) {
    const auto fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0 && errno == EEXIST) return false;
    if (fd < 0) return std::unexpected(Error{ErrorCode::io, "could not reserve archive: " + std::string(strerror(errno))});
    close(fd); return true;
}
std::expected<std::string, Error> read_log_tail(const std::filesystem::path& path) {
    const auto fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return std::unexpected(Error{ErrorCode::io, "could not open log " + path.string()});
    struct stat info {};
    if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode)) { close(fd); return std::unexpected(Error{ErrorCode::invalid_path, "log is not a regular file"}); }
    if (info.st_size > static_cast<off_t>(log_limit)) lseek(fd, info.st_size - log_limit, SEEK_SET);
    auto result = std::string(std::min<std::size_t>(info.st_size, log_limit), '\0');
    auto size = std::size_t{};
    while (size < result.size()) {
        const auto count = read(fd, result.data() + size, result.size() - size);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { close(fd); return std::unexpected(Error{ErrorCode::io, "log read failed"}); }
        if (!count) break;
        size += count;
    }
    close(fd); result.resize(size); return result;
}
std::expected<void, Error> create_archive(const std::filesystem::path& staging, const std::filesystem::path& archive) {
    const auto tar = std::filesystem::exists("/usr/bin/tar") ? "/usr/bin/tar" : "/bin/tar";
    return run_command({tar, "-czf", archive.string(), "-C", staging.string(), "."})
        .transform([](const auto&) {});
}
std::expected<void, Error> show_in_folder(std::string_view archive) {
    const auto path = std::filesystem::path{archive}.parent_path().string();
    return run_command({"/usr/bin/xdg-open", path}).transform([](const auto&) {});
}
}
