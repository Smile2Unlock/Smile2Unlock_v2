#include "rewrite/backend_core.hpp"

#include <iostream>

int main() {
    rewrite::backend::BackendCore backend;
    if (auto init = backend.initialize(); !init) {
        std::cerr << "[su_backendd] initialize failed: " << init.error() << '\n';
        return 1;
    }

    if (auto listeners = backend.start_listeners(); !listeners) {
        std::cerr << "[su_backendd] listener startup failed: " << listeners.error() << '\n';
        return 2;
    }

    std::cout << "[su_backendd] " << backend.status_json() << '\n';
    return 0;
}
