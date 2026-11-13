#include "platform/linux/deploy_client/noctalia_config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace su::deploy;

int failures = 0;

void check(bool ok, std::string_view message) {
    if (!ok) {
        std::cerr << "FAIL noctalia config: " << message << '\n';
        ++failures;
    }
}

std::string read(const std::filesystem::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void write(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

void directory_lookup_order() {
    const auto all = NoctaliaConfigEnvironment{"/n", "/x", "/h"};
    check(noctalia_config_directory(all) == std::filesystem::path("/n"),
          "NOCTALIA_CONFIG_HOME wins");
    check(noctalia_config_directory({std::nullopt, "/x", "/h"})
              == std::filesystem::path("/x/noctalia"),
          "XDG_CONFIG_HOME is next");
    check(noctalia_config_directory({std::nullopt, std::nullopt, "/h"})
              == std::filesystem::path("/h/.config/noctalia"),
          "HOME is the last resort");
    check(!noctalia_config_directory({}), "no base means no directory");
}

void content_rules() {
    const auto content = noctalia_fragment_content();
    check(content.starts_with("# Managed by Smile2Unlock"), "the header names the owner");
    check(content.contains("[lockscreen]") && content.contains("allow_empty_password = true"),
          "the fragment enables empty-password submission");
    check(!content.contains("pam_service"),
          "no pam_service key: Noctalia fixes its service at build time");
}

void write_and_remove(const std::filesystem::path& base) {
    const auto directory = base / "fresh" / "noctalia";
    check(write_noctalia_fragment(directory).has_value(), "the fragment is written");
    const auto path = directory / kNoctaliaFragmentName;
    check(read(path) == noctalia_fragment_content(), "the content is exact");
    check(write_noctalia_fragment(directory).has_value(), "writing twice is idempotent");
    check(remove_noctalia_fragment(directory).has_value() && !std::filesystem::exists(path),
          "our own fragment is removed");
    check(remove_noctalia_fragment(directory).has_value(), "removing twice is idempotent");
    check(std::filesystem::exists(directory), "the config directory itself stays");
}

void refuses_modified(const std::filesystem::path& base) {
    const auto directory = base / "modified";
    const auto path = directory / kNoctaliaFragmentName;
    const auto edited = std::string(noctalia_fragment_content()) + "[bar]\nheight = 40\n";
    write(path, edited);
    const auto overwritten = write_noctalia_fragment(directory);
    check(!overwritten && overwritten.error().contains("modified"), "an edited file is not overwritten");
    const auto removed = remove_noctalia_fragment(directory);
    check(!removed && removed.error().contains("modified"), "an edited file is not deleted");
    check(read(path) == edited, "the edited file survives both attempts");

    const auto linked = base / "linked";
    std::filesystem::create_directories(linked);
    write(base / "elsewhere.toml", noctalia_fragment_content());
    std::filesystem::create_symlink(base / "elsewhere.toml", linked / kNoctaliaFragmentName);
    check(!remove_noctalia_fragment(linked) && std::filesystem::exists(base / "elsewhere.toml"),
          "a symlink is never followed or removed");
}

void exclusive_and_nofollow_writes(const std::filesystem::path& base) {
    const auto directory = base / "hardening";
    std::filesystem::create_directories(directory);
    const auto path = directory / kNoctaliaFragmentName;

    // A symlink already at the destination is refused and never written through.
    write(base / "victim.toml", "victim\n");
    std::filesystem::create_symlink(base / "victim.toml", path);
    const auto linked = write_noctalia_fragment(directory);
    check(!linked && linked.error().contains("not a regular file"),
          "a symlink at the destination is refused");
    check(read(base / "victim.toml") == "victim\n", "the symlink target is untouched");
    std::filesystem::remove(path);

    // A pre-planted temporary name must not be followed or reused (O_EXCL|O_NOFOLLOW).
    const auto temporary = directory / (std::string(".") + std::string(kNoctaliaFragmentName)
                                        + "." + std::to_string(::getpid()) + ".tmp");
    std::filesystem::create_symlink(base / "victim.toml", temporary);
    const auto planted = write_noctalia_fragment(directory);
    check(!planted && !std::filesystem::exists(path), "a planted temporary symlink aborts the write");
    check(read(base / "victim.toml") == "victim\n", "the file behind the temporary symlink is untouched");
    std::filesystem::remove(temporary);

    write(temporary, "stale");
    check(!write_noctalia_fragment(directory) && read(temporary) == "stale",
          "an existing temporary file is never reused");
    std::filesystem::remove(temporary);

    // Anything larger than our own fragment cannot be it and is left alone.
    write(path, std::string(8192, 'x'));
    check(!write_noctalia_fragment(directory) && !remove_noctalia_fragment(directory)
              && read(path) == std::string(8192, 'x'),
          "an oversize file is neither replaced nor removed");
    std::filesystem::remove(path);

    // A FIFO must not hang the read.
    check(::mkfifo(path.c_str(), 0600) == 0, "a FIFO can be planted");
    check(!write_noctalia_fragment(directory) && !remove_noctalia_fragment(directory),
          "a FIFO at the destination is refused without blocking");
    std::filesystem::remove(path);
    check(write_noctalia_fragment(directory).has_value() && read(path) == noctalia_fragment_content(),
          "a clean directory still works afterwards");
}

} // namespace

bool noctalia_config_tests(const std::filesystem::path& base) {
    const auto before = failures;
    directory_lookup_order();
    content_rules();
    write_and_remove(base);
    refuses_modified(base);
    exclusive_and_nofollow_writes(base);
    return failures == before;
}
