#pragma once

#include <cstddef>
#include <string>
#if defined(__linux__)
#include <string.h>
#endif

namespace su::security {

inline void wipe_bytes(void* data, std::size_t size) noexcept {
#if defined(__linux__)
    ::explicit_bzero(data, size);
#else
    auto* bytes = static_cast<volatile unsigned char*>(data);
    while (size-- != 0)
        *bytes++ = 0;
#endif
}

template <typename Buffer> class ScopedWipe {
public:
    explicit ScopedWipe(Buffer& buffer) noexcept : buffer_(buffer) {}
    ~ScopedWipe() { wipe_bytes(buffer_.data(), buffer_.size() * sizeof(*buffer_.data())); }
    ScopedWipe(const ScopedWipe&) = delete;
    ScopedWipe& operator=(const ScopedWipe&) = delete;

private:
    Buffer& buffer_;
};

// Wipe string values (including SSO storage) before the JSON tree is freed.
template <typename Json> void wipe_json(Json& value) noexcept {
    if (value.is_string()) {
        auto& text = value.template get_ref<typename Json::string_t&>();
        wipe_bytes(text.data(), text.size());
    } else if (value.is_structured()) {
        for (auto& child : value)
            wipe_json(child);
    }
}

template <typename Json> class ScopedJsonWipe {
public:
    explicit ScopedJsonWipe(Json& value) noexcept : value_(value) {}
    ~ScopedJsonWipe() { wipe_json(value_); }
    ScopedJsonWipe(const ScopedJsonWipe&) = delete;
    ScopedJsonWipe& operator=(const ScopedJsonWipe&) = delete;

private:
    Json& value_;
};

} // namespace su::security
