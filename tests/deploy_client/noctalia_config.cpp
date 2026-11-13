#include "platform/linux/deploy_client/noctalia_config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

} // namespace

bool noctalia_config_tests(const std::filesystem::path& base) {
    const auto before = failures;
    directory_lookup_order();
    content_rules();
    write_and_remove(base);
    refuses_modified(base);
    return failures == before;
}
