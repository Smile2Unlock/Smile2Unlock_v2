#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace su::windows::security {

enum class KeyProtection : std::uint8_t {
    kTpm2Bound = 1,
    kMachineDpapi = 2,
};

enum class StorageKeyError {
    kUnavailable,
    kUnsafePath,
    kInvalidFormat,
    kRandomFailed,
    kTpmFailed,
    kDpapiFailed,
    kWriteFailed,
    kMemoryLockFailed,
};

class StorageKey {
public:
    StorageKey(const StorageKey&) = delete;
    StorageKey& operator=(const StorageKey&) = delete;
    StorageKey(StorageKey&&) noexcept;
    StorageKey& operator=(StorageKey&&) noexcept;
    ~StorageKey();

    [[nodiscard]] std::span<const std::uint8_t, 32> bytes() const;
    [[nodiscard]] std::uint32_t version() const { return version_; }
    [[nodiscard]] KeyProtection protection() const { return protection_; }

private:
    friend std::expected<StorageKey, StorageKeyError> load_or_create_storage_key(
        const std::filesystem::path& path);
    StorageKey(
        std::unique_ptr<std::array<std::uint8_t, 32>> bytes,
        std::uint32_t version,
        KeyProtection protection);

    std::unique_ptr<std::array<std::uint8_t, 32>> bytes_;
    std::uint32_t version_ = 0;
    KeyProtection protection_ = KeyProtection::kMachineDpapi;
};

std::expected<StorageKey, StorageKeyError> load_or_create_storage_key(
    const std::filesystem::path& path);
std::filesystem::path default_storage_key_path();
std::string_view key_protection_name(KeyProtection protection);
std::string_view storage_key_error_message(StorageKeyError error);

} // namespace su::windows::security
