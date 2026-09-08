#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

import std;
import su.app.user;
import su.auth.user;
import su.core.types;

namespace {

void require(
    bool condition,
    std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::println(
            stderr,
            "multi-user policy assertion failed at {}:{}",
            location.file_name(),
            location.line());
        std::abort();
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(std::filesystem::absolute(std::filesystem::path("build/test-data"))
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
    require(su::auth::peer_request_allowed(1000, Type::kListProfiles, 1000));
    require(su::auth::peer_request_allowed(1000, Type::kEnrollProfile, 1000));
    require(su::auth::peer_request_allowed(1000, Type::kDeleteProfile, 1000));
    require(su::auth::peer_request_allowed(1000, Type::kMigrateProfiles, 1000));
    require(su::auth::peer_request_allowed(1000, Type::kVerifyProfile, 1000));
    require(!su::auth::peer_request_allowed(1000, Type::kAuthenticate, 1001));
    require(!su::auth::peer_request_allowed(1000, Type::kListProfiles, 1001));
    require(!su::auth::peer_request_allowed(1000, Type::kAuthenticate, std::nullopt));
    require(!su::auth::peer_request_allowed(1000, Type::kStatus, std::nullopt));
    require(!su::auth::peer_request_allowed(1000, Type::kCancel, 1000));
    require(!su::auth::peer_request_allowed(
        1000, Type::kIssueManagementCapability, std::nullopt));
}

void assert_management_capabilities() {
    using Operation = su::auth::ManagementOperation;
    auto store = su::auth::ManagementCapabilityStore{};
    const auto now = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};
    auto enroll = store.issue(1000, 2000, 3000, Operation::kEnrollProfile, now);
    require(enroll.has_value());
    require(enroll->size() == 64);
    require(!store.consume(
        *enroll, 1000, 2000, 3000, Operation::kDeleteProfile, now));
    require(!store.consume(
        *enroll, 1000, 2000, 3000, Operation::kEnrollProfile, now));

    auto deletion = store.issue(1000, 2000, 3000, Operation::kDeleteProfile, now);
    require(deletion.has_value());
    require(!store.consume(
        *deletion,
        1000,
        2000,
        3000,
        Operation::kDeleteProfile,
        now + std::chrono::minutes{2}));

    auto valid = store.issue(1000, 2000, 3000, Operation::kMigrateProfiles, now);
    require(valid.has_value());
    require(store.consume(
        *valid, 1000, 2000, 3000, Operation::kMigrateProfiles, now));
    require(!store.consume(
        *valid, 1000, 2000, 3000, Operation::kMigrateProfiles, now));
}

void assert_path_isolation(const std::filesystem::path& root) {
    const auto first = su::auth::paths_for_identity(1000, root / "user-a");
    const auto second = su::auth::paths_for_identity(1001, root / "user-b");
    require(first.uid != second.uid);
    require(first.config != second.config);
    require(first.profiles != second.profiles);
    require(first.config.string().ends_with("user-a/.config/smile2unlock/config.toml"));
    require(first.legacy_profiles.string().ends_with("user-a/.local/share/smile2unlock/profiles.json"));
    require(second.legacy_profiles.string().ends_with("user-b/.local/share/smile2unlock/profiles.json"));
    require(first.profiles == "/var/lib/smile2unlock/users/1000/profiles.s2u");
    require(second.profiles == "/var/lib/smile2unlock/users/1001/profiles.s2u");
    require(first.home == root / "user-a");
}

void assert_identity_lookup() {
    const auto current_name = su::app::current_username("");
    require(!current_name.empty());
    require(su::auth::paths_for_username(current_name).has_value());
    require(!su::auth::paths_for_username("smile2unlock-no-such-user").has_value());
    require(su::auth::user_lookup_error_message(
        su::auth::UserLookupError::kInvalidHome) == "user home is unavailable or unsupported");
    require(!su::app::current_username("fallback").empty());
}

void assert_file_policy(const std::filesystem::path& root) {
    const auto uid = static_cast<std::uint32_t>(::getuid());
    const auto profile_directory = root / ".local/share/smile2unlock";
    std::filesystem::create_directories(profile_directory);
    const auto paths = su::auth::paths_for_identity(uid, root);
    require(!su::auth::open_user_file(paths, su::auth::UserFileKind::kProfiles, true).has_value());
    require(su::auth::open_user_file(paths, su::auth::UserFileKind::kConfig, false).has_value());

    const auto file = paths.legacy_profiles;
    const auto target = root / "target.json";
    const auto link = root / "profile-link.json";
    std::ofstream(target) << "{}\n";
    std::ofstream(file) << "{}\n";
    require(::chmod(file.c_str(), 0600) == 0);
    auto opened = su::auth::open_user_file(paths, su::auth::UserFileKind::kProfiles, true);
    require(opened.has_value() && opened->has_value());

    require(::chmod(file.c_str(), 0660) == 0);
    require(!su::auth::open_user_file(paths, su::auth::UserFileKind::kProfiles, true).has_value());
    require(::chmod(file.c_str(), 0600) == 0);

    std::filesystem::remove(file);
    require(std::filesystem::create_directory(file));
    require(!su::auth::open_user_file(paths, su::auth::UserFileKind::kProfiles, true).has_value());
    std::filesystem::remove(file);

    std::filesystem::create_symlink(target, file);
    require(std::filesystem::is_symlink(file));
    require(!su::auth::open_user_file(paths, su::auth::UserFileKind::kProfiles, true).has_value());
    std::filesystem::remove(file);
    std::filesystem::create_symlink(target, link);
    require(std::filesystem::is_symlink(link));
    auto link_paths = paths;
    link_paths.home = link;
    require(!su::auth::open_user_file(link_paths, su::auth::UserFileKind::kProfiles, true).has_value());

    std::ofstream(file) << "original\n";
    auto pinned = su::auth::open_user_file(paths, su::auth::UserFileKind::kProfiles, true);
    require(pinned.has_value() && pinned->has_value());
    std::filesystem::rename(file, root / "profile-old.json");
    std::ofstream(file) << "replacement\n";
    std::ifstream stable((*pinned)->proc_path());
    require(std::string{std::istreambuf_iterator<char>(stable), {}} == "original\n");

    auto wrong_owner = paths;
    wrong_owner.uid = uid + 1;
    require(!su::auth::open_user_file(wrong_owner, su::auth::UserFileKind::kProfiles, true).has_value());

    std::filesystem::resize_file(file, 16 * 1024 * 1024 + 1);
    require(!su::auth::open_user_file(paths, su::auth::UserFileKind::kProfiles, true).has_value());

    std::filesystem::remove(file);
    std::ofstream(file) << "migrate me\n";
    require(::chmod(file.c_str(), 0600) == 0);
    auto migration_source = su::auth::open_user_file(
        paths, su::auth::UserFileKind::kProfiles, true);
    require(migration_source.has_value() && migration_source->has_value());
    require(su::auth::remove_pinned_user_file(
        paths, su::auth::UserFileKind::kProfiles, **migration_source).has_value());
    require(!std::filesystem::exists(file));
}

void assert_rate_policy() {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{std::chrono::seconds{10}};
    require(!su::auth::authentication_rate_limited(std::nullopt, start, std::chrono::seconds{1}));
    require(su::auth::authentication_rate_limited(start, start, std::chrono::seconds{1}));
    require(su::auth::authentication_rate_limited(
        start, start + std::chrono::milliseconds{999}, std::chrono::seconds{1}));
    require(!su::auth::authentication_rate_limited(
        start, start + std::chrono::seconds{1}, std::chrono::seconds{1}));
}

} // namespace

int main() {
    const auto temporary = TemporaryDirectory{};
    assert_peer_policy();
    assert_management_capabilities();
    assert_path_isolation(temporary.path());
    assert_identity_lookup();
    assert_file_policy(temporary.path());
    assert_rate_policy();
    return 0;
}
