#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

import std;
import su.auth.user;
import su.core.types;

namespace {

void require(bool condition) {
    if (!condition) {
        std::println(stderr, "multi-user policy assertion failed");
        std::abort();
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(std::filesystem::path("build/test-data")
            / std::format("multi-user-{}", ::getpid())) {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        require(!error);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void assert_peer_policy() {
    using Type = su::app::ControlMessageType;
    require(su::auth::peer_request_allowed(0, Type::kAuthenticate, 1001));
    require(su::auth::peer_request_allowed(0, Type::kStatus, std::nullopt));
    require(su::auth::peer_request_allowed(1000, Type::kAuthenticate, 1000));
    require(!su::auth::peer_request_allowed(1000, Type::kAuthenticate, 1001));
    require(!su::auth::peer_request_allowed(1000, Type::kAuthenticate, std::nullopt));
    require(!su::auth::peer_request_allowed(1000, Type::kStatus, std::nullopt));
    require(!su::auth::peer_request_allowed(1000, Type::kCancel, 1000));
}

void assert_path_isolation(const std::filesystem::path& root) {
    const auto first = su::auth::paths_for_identity(1000, root / "user-a");
    const auto second = su::auth::paths_for_identity(1001, root / "user-b");
    require(first.uid != second.uid);
    require(first.config != second.config);
    require(first.profiles != second.profiles);
    require(first.config.string().ends_with("user-a/.config/smile2unlock/config.toml"));
    require(second.profiles.string().ends_with("user-b/.local/share/smile2unlock/profiles.json"));
}

void assert_file_policy(const std::filesystem::path& root) {
    const auto uid = static_cast<std::uint32_t>(::getuid());
    const auto file = root / "profile.json";
    const auto target = root / "target.json";
    const auto link = root / "profile-link.json";
    std::ofstream(target) << "{}\n";
    require(!su::auth::secure_user_file(file, uid, true));
    require(su::auth::secure_user_file(file, uid, false));

    std::ofstream(file) << "{}\n";
    require(::chmod(file.c_str(), 0600) == 0);
    require(su::auth::secure_user_file(file, uid, true));
    require(!su::auth::secure_user_file(file, uid + 1, true));

    require(::chmod(file.c_str(), 0660) == 0);
    require(!su::auth::secure_user_file(file, uid, true));
    require(::chmod(file.c_str(), 0600) == 0);

    std::filesystem::remove(file);
    require(std::filesystem::create_directory(file));
    require(!su::auth::secure_user_file(file, uid, true));
    std::filesystem::remove(file);

    std::filesystem::create_symlink(target, link);
    require(std::filesystem::is_symlink(link));
    require(!su::auth::secure_user_file(link, uid, true));
}

} // namespace

int main() {
    const auto temporary = TemporaryDirectory{};
    assert_peer_policy();
    assert_path_isolation(temporary.path());
    assert_file_policy(temporary.path());
    return 0;
}
