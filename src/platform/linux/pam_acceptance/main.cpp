#include <security/pam_appl.h>
#include <cstdlib>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

import std;
import su.control.socket;

#ifndef SU_PAM_MODULE_PATH
#error "SU_PAM_MODULE_PATH must point to the built PAM module"
#endif

namespace {

constexpr auto kServiceName = std::string_view{"smile2unlock-acceptance"};

struct Options {
    std::string username;
    std::filesystem::path module = SU_PAM_MODULE_PATH;
    std::filesystem::path socket = su::control::kDefaultSocketPath;
    bool help = false;
};

void print_usage(std::string_view program) {
    std::println(
        "Usage: {} --user USER [--module PAM_MODULE] [--socket CONTROL_SOCKET]",
        program);
    std::println("Runs one real PAM authentication without modifying /etc/pam.d.");
    std::println("Non-root callers may authenticate only their own NSS user.");
}

std::expected<Options, std::string> parse_options(int argc, char** argv) {
    auto options = Options{};
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            continue;
        }
        if (argument != "--user" && argument != "--module" && argument != "--socket") {
            return std::unexpected(std::format("unknown argument: {}", argument));
        }
        if (++index >= argc) {
            return std::unexpected(std::format("missing value for {}", argument));
        }
        const auto value = std::string_view(argv[index]);
        if (value.empty()) {
            return std::unexpected(std::format("empty value for {}", argument));
        }
        if (argument == "--user") {
            options.username = value;
        } else if (argument == "--module") {
            options.module = value;
        } else {
            options.socket = value;
        }
    }
    if (!options.help && options.username.empty()) {
        return std::unexpected("--user is required");
    }
    return options;
}

bool valid_pam_token(std::string_view value) {
    return !value.empty()
        && std::ranges::none_of(value, [](unsigned char character) {
            return std::isspace(character) || character == '#';
        });
}

class ConfigDirectory {
public:
    explicit ConfigDirectory(std::filesystem::path path) : path_(std::move(path)) {}
    ~ConfigDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    ConfigDirectory(const ConfigDirectory&) = delete;
    ConfigDirectory& operator=(const ConfigDirectory&) = delete;

private:
    std::filesystem::path path_;
};

std::expected<std::filesystem::path, std::string> write_service_config(
    const Options& options) {
    auto module = std::filesystem::absolute(options.module);
    if (!std::filesystem::is_regular_file(module)) {
        return std::unexpected(std::format("PAM module is not a regular file: {}", module.string()));
    }
    if (!valid_pam_token(module.string()) || !valid_pam_token(options.socket.string())) {
        return std::unexpected("PAM module and socket paths cannot contain whitespace or '#'");
    }

    auto runtime_directory = std::filesystem::path{"/run"};
    if (::geteuid() != 0) {
        const auto* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
        if (xdg_runtime == nullptr || xdg_runtime[0] == '\0') {
            return std::unexpected("XDG_RUNTIME_DIR is required for non-root acceptance tests");
        }
        runtime_directory = xdg_runtime;
        struct stat metadata {};
        if (!runtime_directory.is_absolute()
            || ::stat(runtime_directory.c_str(), &metadata) != 0
            || !S_ISDIR(metadata.st_mode)
            || metadata.st_uid != ::geteuid()
            || (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            return std::unexpected("XDG_RUNTIME_DIR is not a private directory owned by this user");
        }
    }

    const auto directory = runtime_directory
        / std::format("smile2unlock-acceptance-{}", ::getpid());
    std::error_code error;
    if (!std::filesystem::create_directory(directory, error) || error
        || ::chmod(directory.c_str(), 0700) != 0) {
        return std::unexpected(std::format(
            "failed to create temporary PAM directory: {}", directory.string()));
    }

    const auto config_path = directory / std::string(kServiceName);
    auto config = std::ofstream(config_path);
    config << "auth required " << module.string()
           << " socket=" << options.socket.string() << '\n';
    config.close();
    if (!config || ::chmod(config_path.c_str(), 0600) != 0) {
        std::filesystem::remove_all(directory, error);
        return std::unexpected("failed to write temporary PAM service config");
    }
    return directory;
}

int reject_conversation(
    int message_count,
    const pam_message** messages,
    pam_response** responses,
    void* app_data) {
    (void)message_count;
    (void)messages;
    (void)responses;
    (void)app_data;
    return PAM_CONV_ERR;
}

std::string_view result_name(int status) {
    switch (status) {
    case PAM_SUCCESS: return "accepted";
    case PAM_AUTH_ERR: return "rejected";
    case PAM_AUTHINFO_UNAVAIL: return "unavailable";
    default: return "error";
    }
}

int exit_code(int status) {
    switch (status) {
    case PAM_SUCCESS: return 0;
    case PAM_AUTH_ERR: return 2;
    case PAM_AUTHINFO_UNAVAIL: return 3;
    default: return 4;
    }
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    if (!options) {
        std::println(stderr, "{}", options.error());
        print_usage(argc > 0 ? argv[0] : "su_pam_acceptance");
        return 64;
    }
    if (options->help) {
        print_usage(argc > 0 ? argv[0] : "su_pam_acceptance");
        return 0;
    }
    const auto config_directory = write_service_config(*options);
    if (!config_directory) {
        std::println(stderr, "{}", config_directory.error());
        return 1;
    }
    const auto cleanup = ConfigDirectory{*config_directory};
    static const auto conversation = pam_conv{
        .conv = reject_conversation,
        .appdata_ptr = nullptr,
    };
    auto* handle = static_cast<pam_handle_t*>(nullptr);
    auto status = ::pam_start_confdir(
        kServiceName.data(),
        options->username.c_str(),
        &conversation,
        config_directory->c_str(),
        &handle);
    if (status == PAM_SUCCESS) {
        status = ::pam_authenticate(handle, 0);
    }
    const auto* description = ::pam_strerror(handle, status);
    std::println(
        "pam_result={} status={} description=\"{}\"",
        result_name(status),
        status,
        description != nullptr ? description : "unknown");
    if (handle != nullptr) {
        (void)::pam_end(handle, status);
    }
    return exit_code(status);
}
