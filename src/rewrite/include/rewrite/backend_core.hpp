#pragma once

#include <expected>
#include <string>

namespace rewrite::backend {

struct RuntimeState {
    bool initialized;
    bool running;
    std::string summary;
};

struct EndpointSet {
    std::string control;
    std::string events;
    std::string fr_control;
    std::string preview;
};

class BackendCore {
public:
    BackendCore();
    ~BackendCore();

    BackendCore(const BackendCore&) = delete;
    BackendCore& operator=(const BackendCore&) = delete;
    BackendCore(BackendCore&&) noexcept;
    BackendCore& operator=(BackendCore&&) noexcept;

    std::expected<void, std::string> initialize();
    std::expected<void, std::string> start_listeners();
    std::string status_json() const;

    const RuntimeState& state() const noexcept;
    const EndpointSet& endpoints() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace rewrite::backend
