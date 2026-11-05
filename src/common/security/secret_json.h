#pragma once

#include "common/security/sensitive.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <memory>

namespace su::security {

// JSON's lexer, strings, and serializer may reallocate before the final tree
// is wiped. Clear those freed allocations as well, including parse failures.
template <typename T> struct WipingAllocator {
    using value_type = T;
    WipingAllocator() = default;
    template <typename U> WipingAllocator(const WipingAllocator<U>&) noexcept {}
    T* allocate(std::size_t count) { return std::allocator<T>{}.allocate(count); }
    void deallocate(T* data, std::size_t count) noexcept {
        wipe_bytes(data, count * sizeof(T));
        std::allocator<T>{}.deallocate(data, count);
    }
    template <typename U> bool operator==(const WipingAllocator<U>&) const noexcept { return true; }
};

class SecretString : public std::basic_string<char, std::char_traits<char>, WipingAllocator<char>> {
    using Base = std::basic_string<char, std::char_traits<char>, WipingAllocator<char>>;

public:
    using Base::Base;
    using Base::operator=;
    SecretString() = default;
    SecretString(const SecretString&) = default;
    SecretString(SecretString&& other) : Base(other) {} // retain size so source SSO is wiped too
    operator std::string() const {
        // Use heap storage even for short payloads: moving an SSO string can
        // otherwise leave secret bytes in the moved-from object's inline area.
        auto result = std::string{};
        result.reserve(std::max(size(), std::size_t{64}));
        result.assign(data(), size());
        return result;
    }
    void clear() noexcept {
        wipe_bytes(data(), size());
        Base::clear();
    }
    void resize(std::size_t count) {
        if (count < size())
            wipe_bytes(data() + count, size() - count);
        Base::resize(count);
    }
    void resize(std::size_t count, char character) {
        if (count < size())
            wipe_bytes(data() + count, size() - count);
        Base::resize(count, character);
    }
    SecretString& operator=(const SecretString& other) {
        if (this != &other) {
            wipe_bytes(data(), size());
            Base::operator=(other);
        }
        return *this;
    }
    SecretString& operator=(SecretString&& other) { return operator=(other); }
    ~SecretString() { wipe_bytes(data(), size()); }
};

using SecretJson = nlohmann::basic_json<std::map, std::vector, SecretString, bool, std::int64_t,
                                        std::uint64_t, double, WipingAllocator>;

inline std::string secret_json_dump(const SecretJson& value) {
    const auto serialized = value.dump();
    return static_cast<std::string>(serialized);
}

} // namespace su::security
