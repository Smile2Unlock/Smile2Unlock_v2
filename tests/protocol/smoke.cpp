#include "app/core_bridge.h"

#include <cassert>

int main() {
    const auto threshold = su::app::default_threshold();
    assert(threshold.has_value());
    assert(*threshold > 0.0F);

    const auto decision = su::app::evaluate_auth("demo", 0.72F, *threshold, true);
    assert(decision.has_value());
    assert(decision->accepted);
    return 0;
}
