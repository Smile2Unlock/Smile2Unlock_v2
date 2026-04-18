#include "rewrite/backend_core.hpp"

#include <iostream>

int main() {
    rewrite::backend::BackendCore backend;
    if (auto init = backend.initialize(); !init) {
        std::cerr << "[su_facerecognizer] bootstrap failed: " << init.error() << '\n';
        return 1;
    }

    std::cout << "[su_facerecognizer] worker skeleton online\n";
    std::cout << "{\"command\":\"START_PREVIEW\",\"codec\":\"jpeg\"}\n";
    return 0;
}
