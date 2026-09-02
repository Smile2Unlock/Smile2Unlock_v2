module;

#include <fcntl.h>
#include <cerrno>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

export module su.auth.storage;

import std;
import su.core.types;

export namespace su::auth {

enum class KeyProtection {
    kTpm2Bound = 1,
    kHostKey = 2,
};

enum class KeyProviderError {
    kUnavailable,
    kUnsafeFile,
    kInvalidFormat,
    kMemoryLockFailed,
};

class MasterKey {
public:
    MasterKey(const MasterKey&) = delete;
    MasterKey& operator=(const MasterKey&) = delete;
    MasterKey(MasterKey&&) noexcept = default;
    MasterKey& operator=(MasterKey&&) noexcept = default;
    ~MasterKey() = default;

    [[nodiscard]] KeyProtection protection() const { return protection_; }
    [[nodiscard]] std::uint32_t version() const { return version_; }
    [[nodiscard]] std::span<const std::uint8_t, 32> bytes() const;
    [[nodiscard]] su::app::EncryptedStoreContext context_for_uid(std::uint32_t uid) const;

private:
    struct LockedBufferDeleter {
        std::size_t size = 0;
        void operator()(std::uint8_t* buffer) const noexcept;
    };

    using LockedBuffer = std::unique_ptr<std::uint8_t[], LockedBufferDeleter>;
    friend std::expected<MasterKey, KeyProviderError> load_key_credential(
        const std::filesystem::path& path);
    MasterKey(LockedBuffer bytes, KeyProtection protection, std::uint32_t version)
        : bytes_(std::move(bytes)), protection_(protection), version_(version) {}

    LockedBuffer bytes_;
    KeyProtection protection_ = KeyProtection::kHostKey;
    std::uint32_t version_ = 0;
};

std::expected<MasterKey, KeyProviderError> load_key_credential(
    const std::filesystem::path& path);
std::expected<MasterKey, KeyProviderError> load_systemd_key_credential();
std::string_view key_protection_name(KeyProtection protection);
std::string_view key_provider_error_message(KeyProviderError error);

} // namespace su::auth

namespace su::auth {

namespace {

constexpr auto kCredentialSize = std::size_t{44};
constexpr auto kMasterKeyOffset = std::size_t{12};
constexpr auto kMasterKeySize = std::size_t{32};
constexpr auto kCredentialName = std::string_view{"smile2unlock-master.key"};

void clear_bytes(std::span<std::uint8_t> bytes) noexcept {
    auto* cursor = reinterpret_cast<volatile std::uint8_t*>(bytes.data());
    for (auto index = std::size_t{0}; index < bytes.size(); ++index) {
        cursor[index] = 0;
    }
}

bool read_exact(int fd, std::span<std::uint8_t> output) {
    auto consumed = std::size_t{0};
    while (consumed < output.size()) {
        const auto result = ::read(fd, output.data() + consumed, output.size() - consumed);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        consumed += static_cast<std::size_t>(result);
    }
    return true;
}

std::uint32_t read_u32_be(std::span<const std::uint8_t, 4> bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24)
        | (static_cast<std::uint32_t>(bytes[1]) << 16)
        | (static_cast<std::uint32_t>(bytes[2]) << 8)
        | static_cast<std::uint32_t>(bytes[3]);
}

} // namespace

void MasterKey::LockedBufferDeleter::operator()(std::uint8_t* buffer) const noexcept {
    if (buffer == nullptr) {
        return;
    }
    clear_bytes(std::span{buffer, size});
    (void)::munlock(buffer, size);
    delete[] buffer;
}

std::span<const std::uint8_t, 32> MasterKey::bytes() const {
    return std::span<const std::uint8_t, 32>{bytes_.get() + kMasterKeyOffset, kMasterKeySize};
}

su::app::EncryptedStoreContext MasterKey::context_for_uid(std::uint32_t uid) const {
    return su::app::EncryptedStoreContext{
        .master_key = bytes(),
        .key_version = version_,
        .account = uid,
    };
}

std::expected<MasterKey, KeyProviderError> load_key_credential(
    const std::filesystem::path& path) {
    const auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return std::unexpected(KeyProviderError::kUnavailable);
    }
    const auto close_fd = std::unique_ptr<int, decltype([](int* value) {
        if (value != nullptr) {
            (void)::close(*value);
            delete value;
        }
    })>{new int(fd)};

    struct stat metadata {};
    if (::fstat(fd, &metadata) != 0
        || !S_ISREG(metadata.st_mode)
        || metadata.st_uid != ::geteuid()
        || (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0
        || metadata.st_size != static_cast<off_t>(kCredentialSize)) {
        return std::unexpected(KeyProviderError::kUnsafeFile);
    }

    auto buffer = MasterKey::LockedBuffer{
        new (std::nothrow) std::uint8_t[kCredentialSize],
        MasterKey::LockedBufferDeleter{.size = kCredentialSize},
    };
    if (!buffer) {
        return std::unexpected(KeyProviderError::kUnavailable);
    }
    if (::mlock(buffer.get(), kCredentialSize) != 0) {
        return std::unexpected(KeyProviderError::kMemoryLockFailed);
    }
    if (!read_exact(fd, std::span{buffer.get(), kCredentialSize})) {
        return std::unexpected(KeyProviderError::kUnavailable);
    }

    const auto bytes = std::span<const std::uint8_t, kCredentialSize>{
        buffer.get(), kCredentialSize};
    if (!std::ranges::equal(bytes.first<4>(), std::string_view{"S2UK"})
        || bytes[4] != 0
        || bytes[5] != 1
        || bytes[7] != 0) {
        return std::unexpected(KeyProviderError::kInvalidFormat);
    }
    const auto protection = bytes[6] == static_cast<std::uint8_t>(KeyProtection::kTpm2Bound)
        ? std::optional{KeyProtection::kTpm2Bound}
        : bytes[6] == static_cast<std::uint8_t>(KeyProtection::kHostKey)
            ? std::optional{KeyProtection::kHostKey}
            : std::nullopt;
    const auto version = read_u32_be(bytes.subspan<8, 4>());
    if (!protection || version == 0) {
        return std::unexpected(KeyProviderError::kInvalidFormat);
    }
    return MasterKey{std::move(buffer), *protection, version};
}

std::expected<MasterKey, KeyProviderError> load_systemd_key_credential() {
    const auto* directory = std::getenv("CREDENTIALS_DIRECTORY");
    if (directory == nullptr || directory[0] != '/') {
        return std::unexpected(KeyProviderError::kUnavailable);
    }
    return load_key_credential(std::filesystem::path{directory} / kCredentialName);
}

std::string_view key_protection_name(KeyProtection protection) {
    switch (protection) {
    case KeyProtection::kTpm2Bound: return "TPM2-bound";
    case KeyProtection::kHostKey: return "host-key";
    }
    std::unreachable();
}

std::string_view key_provider_error_message(KeyProviderError error) {
    switch (error) {
    case KeyProviderError::kUnavailable: return "credential unavailable";
    case KeyProviderError::kUnsafeFile: return "credential file is unsafe";
    case KeyProviderError::kInvalidFormat: return "credential format is invalid";
    case KeyProviderError::kMemoryLockFailed: return "credential memory lock failed";
    }
    std::unreachable();
}

} // namespace su::auth
