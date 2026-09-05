#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace su::windows::auth_service {

class TokenBucketBySid {
public:
    using Clock = std::chrono::steady_clock;

    struct Config {
        std::size_t capacity;
        std::chrono::milliseconds refill_period;
        std::chrono::minutes idle_ttl{10};
        std::size_t maximum_sids{1024};
    };

    explicit TokenBucketBySid(Config config) : config_(config) {}

    bool allow(std::wstring_view sid, Clock::time_point now = Clock::now()) {
        if (sid.empty() || config_.capacity == 0 || config_.refill_period.count() <= 0) {
            return false;
        }
        auto entry = buckets_.find(std::wstring{sid});
        if (entry == buckets_.end()) {
            prune(now);
            if (buckets_.size() >= config_.maximum_sids) {
                return false;
            }
            entry = buckets_.emplace(
                std::wstring{sid}, Bucket{config_.capacity, now, now}).first;
        }
        auto& bucket = entry->second;
        const auto elapsed = now - bucket.last_refill;
        if (elapsed >= config_.refill_period) {
            const auto replenished = static_cast<std::size_t>(elapsed / config_.refill_period);
            bucket.tokens = std::min(config_.capacity, bucket.tokens + replenished);
            bucket.last_refill += config_.refill_period * replenished;
        }
        bucket.last_seen = now;
        if (bucket.tokens == 0) {
            return false;
        }
        --bucket.tokens;
        return true;
    }

private:
    struct Bucket {
        std::size_t tokens;
        Clock::time_point last_refill;
        Clock::time_point last_seen;
    };

    void prune(Clock::time_point now) {
        std::erase_if(buckets_, [&](const auto& item) {
            return now - item.second.last_seen >= config_.idle_ttl;
        });
    }

    Config config_;
    std::unordered_map<std::wstring, Bucket> buckets_;
};

class SidRateLimiter {
public:
    using Clock = TokenBucketBySid::Clock;

    SidRateLimiter()
        : connections_({.capacity = 6, .refill_period = std::chrono::seconds{1}}),
          requests_({.capacity = 20, .refill_period = std::chrono::milliseconds{200}}) {}

    bool allow_connection(std::wstring_view sid, Clock::time_point now = Clock::now()) {
        return connections_.allow(sid, now);
    }

    bool allow_request(std::wstring_view sid, Clock::time_point now = Clock::now()) {
        return requests_.allow(sid, now);
    }

private:
    TokenBucketBySid connections_;
    TokenBucketBySid requests_;
};

} // namespace su::windows::auth_service
