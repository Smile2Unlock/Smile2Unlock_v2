#pragma once

#include "storage_key_provider.h"
#include "su_core.h"

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace su::windows::security {

enum class LogonSecretError {
    kInvalidArgument,
    kUnavailable,
    kAccessDenied,
    kCorrupt,
    kWriteFailed,
};

class PreparedLogonSecret {
public:
    PreparedLogonSecret(const PreparedLogonSecret&) = delete;
    PreparedLogonSecret& operator=(const PreparedLogonSecret&) = delete;
    PreparedLogonSecret(PreparedLogonSecret&& other) noexcept;
    PreparedLogonSecret& operator=(PreparedLogonSecret&& other) noexcept;
    ~PreparedLogonSecret();

    [[nodiscard]] const wchar_t* c_str() const;
    [[nodiscard]] std::size_t size() const { return size_; }

private:
    friend class LogonSecretStore;
    PreparedLogonSecret() = default;
    void clear() noexcept;

    std::array<std::uint16_t, 513> password_{};
    std::size_t size_ = 0;
};

class LogonSecretStore {
public:
    explicit LogonSecretStore(const StorageKey& storage_key)
        : storage_key_(storage_key) {}

    std::expected<std::uint64_t, LogonSecretError> store(
        std::string_view sid,
        std::string_view canonical_username,
        SuWindowsAccountKind account_kind,
        std::span<const wchar_t> password) const;
    std::expected<PreparedLogonSecret, LogonSecretError> prepare(
        std::string_view sid,
        std::uint64_t request_id,
        std::uint32_t logon_session_id) const;
    std::expected<void, LogonSecretError> mark_stale(std::string_view sid) const;
    std::expected<bool, LogonSecretError> clear(std::string_view sid) const;
    std::expected<bool, LogonSecretError> configured(std::string_view sid) const;

private:
    std::expected<std::filesystem::path, LogonSecretError> secret_path(
        std::string_view sid) const;
    SuEncryptedStoreContext context(const char* sid) const;

    const StorageKey& storage_key_;
};

std::string_view logon_secret_error_message(LogonSecretError error);

} // namespace su::windows::security
