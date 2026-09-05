#include "sid_rate_limiter.h"

#include <chrono>
#include <iostream>

int main() {
    using Limiter = su::windows::auth_service::SidRateLimiter;
    const auto start = Limiter::Clock::time_point{};
    auto limiter = Limiter{};

    for (int attempt = 0; attempt < 6; ++attempt) {
        if (!limiter.allow_connection(L"S-1-5-21-1000", start)) {
            std::cerr << "connection burst rejected too early\n";
            return 1;
        }
    }
    if (limiter.allow_connection(L"S-1-5-21-1000", start)) {
        std::cerr << "connection burst was not limited\n";
        return 1;
    }
    if (!limiter.allow_connection(L"S-1-5-21-2000", start)) {
        std::cerr << "one SID affected another SID\n";
        return 1;
    }
    if (!limiter.allow_connection(
            L"S-1-5-21-1000", start + std::chrono::seconds{1})) {
        std::cerr << "connection token did not refill\n";
        return 1;
    }

    for (int attempt = 0; attempt < 20; ++attempt) {
        if (!limiter.allow_request(L"S-1-5-21-1000", start)) {
            std::cerr << "request burst rejected too early\n";
            return 1;
        }
    }
    if (limiter.allow_request(L"S-1-5-21-1000", start)) {
        std::cerr << "request burst was not limited\n";
        return 1;
    }
    if (!limiter.allow_request(
            L"S-1-5-21-1000", start + std::chrono::milliseconds{200})) {
        std::cerr << "request token did not refill\n";
        return 1;
    }
    std::cout << "sid rate limiter: ok\n";
    return 0;
}
