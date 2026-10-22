#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace su::windows::auth_service {
// Management tokens are issued and consumed from pool worker threads; every
// entry point takes the same mutex so the grant map needs no external lock.
class ManagementAuthorizer {
public:
    static constexpr auto kTokenBytes = std::size_t{32};
    static constexpr auto kTokenCharacters = kTokenBytes * 2;

    ~ManagementAuthorizer() {
        for (auto& [_, grant] : grants_) {
            SecureZeroMemory(grant.token.data(), grant.token.size());
        }
    }

    bool issue(std::wstring_view sid, DWORD process_id, wchar_t* output, std::size_t capacity) {
        const auto lock = std::lock_guard{mutex_};
        if (sid.empty() || process_id == 0 || capacity <= kTokenCharacters) {
            return false;
        }
        auto token = std::array<std::uint8_t, kTokenBytes>{};
        if (BCryptGenRandom(nullptr, token.data(), static_cast<ULONG>(token.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            return false;
        }
        static constexpr wchar_t kHex[] = L"0123456789abcdef";
        for (auto index = std::size_t{0}; index < token.size(); ++index) {
            output[index * 2] = kHex[token[index] >> 4];
            output[index * 2 + 1] = kHex[token[index] & 0x0F];
        }
        output[kTokenCharacters] = L'\0';
        prune_expired();
        const auto grant_key = key(sid, process_id);
        if (const auto existing = grants_.find(grant_key); existing != grants_.end()) {
            SecureZeroMemory(existing->second.token.data(), existing->second.token.size());
            grants_.erase(existing);
        }
        grants_.insert_or_assign(grant_key, Grant{
                                                .token = token,
                                                .expires_at = std::chrono::steady_clock::now() +
                                                              std::chrono::minutes{10},
                                            });
        SecureZeroMemory(token.data(), token.size());
        return true;
    }

    bool consume(std::wstring_view sid, DWORD process_id, std::wstring_view encoded_token) {
        const auto lock = std::lock_guard{mutex_};
        if (encoded_token.size() != kTokenCharacters) {
            return false;
        }
        auto token = decode(encoded_token);
        if (!token) {
            return false;
        }
        prune_expired();
        const auto grant = grants_.find(key(sid, process_id));
        if (grant == grants_.end()) {
            SecureZeroMemory(token->data(), token->size());
            return false;
        }
        auto difference = std::uint8_t{0};
        for (auto index = std::size_t{0}; index < token->size(); ++index) {
            difference |= (*token)[index] ^ grant->second.token[index];
        }
        SecureZeroMemory(token->data(), token->size());
        if (difference != 0) {
            return false;
        }
        SecureZeroMemory(grant->second.token.data(), grant->second.token.size());
        grants_.erase(grant);
        return true;
    }

private:
    struct Grant {
        std::array<std::uint8_t, kTokenBytes> token{};
        std::chrono::steady_clock::time_point expires_at;
    };

    static std::wstring key(std::wstring_view sid, DWORD process_id) {
        return std::wstring{sid} + L":" + std::to_wstring(process_id);
    }

    static std::optional<std::array<std::uint8_t, kTokenBytes>> decode(std::wstring_view encoded) {
        const auto nibble = [](wchar_t value) -> std::optional<std::uint8_t> {
            if (value >= L'0' && value <= L'9')
                return value - L'0';
            if (value >= L'a' && value <= L'f')
                return value - L'a' + 10;
            if (value >= L'A' && value <= L'F')
                return value - L'A' + 10;
            return std::nullopt;
        };
        auto token = std::array<std::uint8_t, kTokenBytes>{};
        for (auto index = std::size_t{0}; index < token.size(); ++index) {
            const auto high = nibble(encoded[index * 2]);
            const auto low = nibble(encoded[index * 2 + 1]);
            if (!high || !low) {
                SecureZeroMemory(token.data(), token.size());
                return std::nullopt;
            }
            token[index] = static_cast<std::uint8_t>((*high << 4) | *low);
        }
        return token;
    }

    void prune_expired() {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(grants_, [now](auto& item) {
            if (item.second.expires_at > now) {
                return false;
            }
            SecureZeroMemory(item.second.token.data(), item.second.token.size());
            return true;
        });
    }

    std::mutex mutex_;
    std::unordered_map<std::wstring, Grant> grants_;
};

} // namespace su::windows::auth_service
