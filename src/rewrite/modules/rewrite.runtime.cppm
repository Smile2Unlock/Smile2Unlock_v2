export module rewrite.runtime;

import std;

export namespace rewrite::runtime {

struct EndpointSet {
    std::string control;
    std::string events;
    std::string fr_control;
    std::string preview;
};

[[nodiscard]] inline std::string configured_run_root() {
    if (const char* env = std::getenv("SMILE2UNLOCK_RUNROOT")) {
        if (*env != '\0') {
            return env;
        }
    }
#ifdef _WIN32
    return "127.0.0.1";
#else
    return "/run/smile2unlock";
#endif
}

[[nodiscard]] inline EndpointSet default_endpoints() {
#ifdef _WIN32
    return EndpointSet{
        .control = "127.0.0.1:43100",
        .events = "127.0.0.1:43101",
        .fr_control = "127.0.0.1:43110",
        .preview = "127.0.0.1:43111"
    };
#else
    const std::string root = configured_run_root();
    return EndpointSet{
        .control = root + "/control.sock",
        .events = root + "/events.sock",
        .fr_control = root + "/fr-control.sock",
        .preview = root + "/preview.sock"
    };
#endif
}

[[nodiscard]] inline std::filesystem::path auth_tag_path() {
#ifdef _WIN32
    if (const char* env = std::getenv("LOCALAPPDATA")) {
        return std::filesystem::path(env) / "Smile2Unlock" / "auth.tag";
    }
    return std::filesystem::temp_directory_path() / "Smile2Unlock" / "auth.tag";
#else
    return std::filesystem::path(configured_run_root()) / "auth.tag";
#endif
}

inline std::expected<void, std::string> ensure_runtime_dirs() {
#ifdef _WIN32
    const auto root = auth_tag_path().parent_path();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        return std::unexpected(std::format("failed to create runtime directory: {}", ec.message()));
    }
    return {};
#else
    const std::filesystem::path root = configured_run_root();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        return std::unexpected(std::format("failed to create runtime directory: {}", ec.message()));
    }
    return {};
#endif
}

} // namespace rewrite::runtime
