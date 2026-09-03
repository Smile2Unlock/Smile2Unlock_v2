#pragma once

#include "storage_key_provider.h"
#include "su_core.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace su::windows::security {

enum class FaceProfileStoreError {
    kInvalidArgument,
    kUnavailable,
    kCorrupt,
    kWriteFailed,
    kNotFound,
};

class FaceProfileStore {
public:
    explicit FaceProfileStore(const StorageKey& storage_key)
        : storage_key_(storage_key) {}

    std::expected<void, FaceProfileStoreError> enroll(
        std::string_view sid,
        std::string_view label,
        std::string_view embedding_source) const;
    std::expected<bool, FaceProfileStoreError> remove(
        std::string_view sid,
        std::string_view profile_id) const;
    std::expected<std::string, FaceProfileStoreError> list_json(
        std::string_view sid) const;
    std::expected<SuFaceAuthReport, FaceProfileStoreError> authenticate(
        std::string_view sid,
        std::string_view embedding_source,
        float threshold,
        bool liveness_ok) const;

private:
    std::expected<std::filesystem::path, FaceProfileStoreError> profile_path(
        std::string_view sid) const;
    SuEncryptedStoreContext context(const char* sid) const;

    const StorageKey& storage_key_;
};

} // namespace su::windows::security
