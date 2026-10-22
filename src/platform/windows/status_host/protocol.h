#pragma once
#include <atomic>
#include <cstdint>

namespace su::status {
// Matches credential_provider_rs/src/status_bridge.rs. This mapping is
// presentation-only and can never authorize a login.
struct alignas(8) SharedState {
    std::uint32_t magic;
    std::uint32_t version;
    std::atomic<std::uint32_t> state;
    std::uint32_t reserved;
    std::atomic<std::uint64_t> heartbeat;
};
static_assert(sizeof(SharedState) == 24);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

enum class Phase : std::uint32_t {
    hidden, waiting, initial_delay, recognizing, retry_delay, ready,
    submitting, no_match, timed_out, unavailable, login_failed,
};
struct Snapshot {
    Phase phase;
    bool automatic;
    unsigned seconds;
    bool last_no_face = false;
};
// Matches status.rs: phase 0–3, automatic 4, last-no-face 5, seconds 8–23.
inline constexpr std::uint32_t kLastNoFace = 1U << 5;
constexpr std::uint32_t encode(Snapshot state) {
    return static_cast<std::uint32_t>(state.phase) | (state.automatic ? 16U : 0U)
        | (state.last_no_face ? kLastNoFace : 0U) | (state.seconds << 8);
}
constexpr Snapshot decode(std::uint32_t packed) {
    const auto phase = packed & 15U;
    if (phase > static_cast<unsigned>(Phase::login_failed) || (packed & 0xff0000c0U)) {
        return {Phase::hidden, false, 0};
    }
    return {static_cast<Phase>(phase), (packed & 16U) != 0, (packed >> 8) & 65535U,
        (packed & kLastNoFace) != 0};
}
// Contrast crossover for opaque black/white on linear relative luminance.
// The wider switching band keeps slowly changing backgrounds from flickering.
constexpr bool use_dark_foreground(double luminance, bool was_dark, bool initial) {
    return initial ? luminance > 0.179 : (was_dark ? luminance >= 0.14 : luminance > 0.22);
}
} // namespace su::status
