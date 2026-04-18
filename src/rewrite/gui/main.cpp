#include "rewrite/backend_core.hpp"

#include <iostream>

int main() {
    rewrite::backend::BackendCore backend;
    if (auto init = backend.initialize(); !init) {
        std::cerr << "[su_gui] backend bootstrap failed: " << init.error() << '\n';
        return 1;
    }

    std::cout << "[su_gui] rewrite GUI host ready\n";
    std::cout << backend.status_json() << '\n';
    return 0;
}
