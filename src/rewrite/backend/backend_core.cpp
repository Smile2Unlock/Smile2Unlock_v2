#include "rewrite/backend_core.hpp"

#include <format>
#include <utility>

import rewrite.protocol;
import rewrite.runtime;
import rewrite.socket;

namespace rewrite::backend {

struct BackendCore::Impl {
    rewrite::runtime::EndpointSet runtime_endpoints{rewrite::runtime::default_endpoints()};
    EndpointSet public_endpoints{
        .control = runtime_endpoints.control,
        .events = runtime_endpoints.events,
        .fr_control = runtime_endpoints.fr_control,
        .preview = runtime_endpoints.preview
    };
    RuntimeState state{false, false, {}};
    rewrite::net::SocketServer control_server{{runtime_endpoints.control}};
    rewrite::net::SocketServer event_server{{runtime_endpoints.events}};
    rewrite::net::SocketServer fr_control_server{{runtime_endpoints.fr_control}};
    rewrite::net::SocketServer preview_server{{runtime_endpoints.preview}};
};

BackendCore::BackendCore()
    : impl_(new Impl()) {}

BackendCore::~BackendCore() {
    delete impl_;
}

BackendCore::BackendCore(BackendCore&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr)) {}

BackendCore& BackendCore::operator=(BackendCore&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

std::expected<void, std::string> BackendCore::initialize() {
    if (impl_->state.initialized) {
        return {};
    }

    if (auto dirs = rewrite::runtime::ensure_runtime_dirs(); !dirs) {
        return std::unexpected(dirs.error());
    }

    impl_->state.initialized = true;
    impl_->state.summary = "rewrite backend core initialized";
    return {};
}

std::expected<void, std::string> BackendCore::start_listeners() {
    if (!impl_->state.initialized) {
        return std::unexpected("backend core is not initialized");
    }

    if (auto control = impl_->control_server.bind_and_listen(); !control) {
        return std::unexpected(std::format("control listener failed: {}", control.error()));
    }
    if (auto events = impl_->event_server.bind_and_listen(); !events) {
        return std::unexpected(std::format("event listener failed: {}", events.error()));
    }
    if (auto fr = impl_->fr_control_server.bind_and_listen(); !fr) {
        return std::unexpected(std::format("fr-control listener failed: {}", fr.error()));
    }
    if (auto preview = impl_->preview_server.bind_and_listen(); !preview) {
        return std::unexpected(std::format("preview listener failed: {}", preview.error()));
    }

    impl_->state.running = true;
    impl_->state.summary = "rewrite backend listeners active";
    return {};
}

std::string BackendCore::status_json() const {
    return std::format(
        "{{\"initialized\":{},\"running\":{},\"control\":\"{}\",\"events\":\"{}\",\"fr_control\":\"{}\",\"preview\":\"{}\",\"summary\":\"{}\"}}",
        impl_->state.initialized ? "true" : "false",
        impl_->state.running ? "true" : "false",
        impl_->public_endpoints.control,
        impl_->public_endpoints.events,
        impl_->public_endpoints.fr_control,
        impl_->public_endpoints.preview,
        impl_->state.summary);
}

const RuntimeState& BackendCore::state() const noexcept {
    return impl_->state;
}

const EndpointSet& BackendCore::endpoints() const noexcept {
    return impl_->public_endpoints;
}

} // namespace rewrite::backend
