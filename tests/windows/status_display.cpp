#include "platform/windows/status_host/protocol.h"
#include <cstdlib>
#include <iostream>

int main() {
    using namespace su::status;
    const auto check = [](bool condition) { if (!condition) std::abort(); };
    // Real wire examples from the producer, including the automatic bit and
    // full countdown range. Corrupted/reserved bits must never paint success.
    const auto delay = decode(0x00000312);
    check(delay.phase == Phase::initial_delay && delay.automatic && delay.seconds == 3);
    check(decode(0x00ffff14).seconds == 65535);
    check(decode(0x01000005).phase == Phase::hidden);
    check(decode(0x00000025).phase == Phase::hidden);
    check(decode(0x0000000f).phase == Phase::hidden);
    check(decode(5).phase == Phase::ready);
    check(decode(6).phase == Phase::submitting);
    // Dark and light backgrounds choose opposite foregrounds; a slow change
    // near the contrast crossover must preserve the previous choice.
    check(!use_dark_foreground(0.01, true, true));
    check(use_dark_foreground(0.95, false, true));
    for (double sample : {0.16, 0.18, 0.20, 0.17}) {
        check(use_dark_foreground(sample, true, false));
        check(!use_dark_foreground(sample, false, false));
    }
    check(!use_dark_foreground(0.13, true, false));
    check(use_dark_foreground(0.23, false, false));
    std::cout << "status protocol and contrast tests passed\n";
}
