module;

#include <fcntl.h>
#include <cerrno>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <cstdlib>
#endif

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
    // RAII read access: temporarily elevates the mprotect state of the locked
    // page from PROT_NONE to PROT_READ, hands the callback the 32 key bytes,
    // and re-seals to PROT_NONE when the guard drops. Idle the key is
    // unreadable and excluded from core dumps (MADV_DONTDUMP on Linux) —
    // mirroring memsafe's Unix idle-seal. The page stays readable for exactly
    // the duration of `fn`, so no key-bearing span can outlive the seal.
    template <typename Fn>
    [[nodiscard]] auto with_bytes(Fn&& fn) const {
        const auto access = std::scoped_lock{*access_mutex_};
        elevate();
        SealGuard guard{page_.get()};
        return std::forward<Fn>(fn)(bytes());
    }
    // Runs `fn` with a fully-constructed EncryptedStoreContext while the key
    // page is elevated to readable, then re-seals. This is the only way to get
    // an EncryptedStoreContext from a sealed MasterKey; the context (and any
    // span it carries) is guaranteed to live within the call.
    template <typename Fn>
    [[nodiscard]] auto with_context(std::uint32_t uid, Fn&& fn) const {
        const auto access = std::scoped_lock{*access_mutex_};
        elevate();
        SealGuard guard{page_.get()};
        const su::app::EncryptedStoreContext context{
            .master_key = bytes(),
            .key_version = version_,
            .account = uid,
        };
        return std::forward<Fn>(fn)(context);
    }

private:
    // Page-aligned mmap mapping (memsafe-style). Deleter wipes the page and
    // munmaps it; mlock/MADV_DONTDUMP/mprotect are handled in load/guards.
    struct PageDeleter {
        std::size_t length = 0;
        void operator()(std::uint8_t* page) const noexcept;
    };

    using Page = std::unique_ptr<std::uint8_t[], PageDeleter>;
    friend std::expected<MasterKey, KeyProviderError> load_key_credential(
        const std::filesystem::path& path);
    MasterKey(Page page, KeyProtection protection, std::uint32_t version)
        : page_(std::move(page)), protection_(protection), version_(version) {}

    [[nodiscard]] std::span<const std::uint8_t, 32> bytes() const;
    void elevate() const;
    // Restores PROT_NONE on the locked page when a with_bytes() read ends.
    struct SealGuard {
        std::uint8_t* page;
        ~SealGuard() noexcept {
            (void)::mprotect(page, kSealLength, PROT_NONE);
        }
    };

    static constexpr std::size_t kSealLength = 4096;

    Page page_;
    // The page protection applies to the whole mapping, so readers must not
    // independently elevate/reseal it from different connection threads.
    std::shared_ptr<std::mutex> access_mutex_ = std::make_shared<std::mutex>();
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

void MasterKey::PageDeleter::operator()(std::uint8_t* page) const noexcept {
    if (page == nullptr) {
        return;
    }
    // The page may be sealed (PROT_NONE); elevate before wiping so the
    // volatile-clearing loop can touch it, then mlock is released on a page
    // whose protection we restored (munlock works on any mapped region).
    (void)::mprotect(page, length, PROT_READ | PROT_WRITE);
    clear_bytes(std::span{page, length});
    (void)::munlock(page, length);
    (void)::munmap(page, length);
}

std::span<const std::uint8_t, 32> MasterKey::bytes() const {
    return std::span<const std::uint8_t, 32>{
        page_.get() + kMasterKeyOffset, kMasterKeySize};
}

void MasterKey::elevate() const {
    (void)::mprotect(page_.get(), kSealLength, PROT_READ);
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

    // Page-aligned mapping (memsafe-style) so mprotect can seal/protect the
    // whole page; mlock pins it (no swap-out), MADV_DONTDUMP keeps it out of
    // core dumps.
    auto* mapping = static_cast<std::uint8_t*>(
        ::mmap(nullptr, MasterKey::kSealLength, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (mapping == MAP_FAILED) {
        return std::unexpected(KeyProviderError::kUnavailable);
    }
    auto page = MasterKey::Page{
        mapping, MasterKey::PageDeleter{.length = MasterKey::kSealLength}};
    if (::mlock(mapping, MasterKey::kSealLength) != 0) {
        return std::unexpected(KeyProviderError::kMemoryLockFailed);
    }
#if defined(__linux__)
    // Key never lands in a core dump.
    (void)::madvise(mapping, MasterKey::kSealLength, MADV_DONTDUMP);
#endif

    if (!read_exact(fd, std::span{mapping, kCredentialSize})) {
        return std::unexpected(KeyProviderError::kUnavailable);
    }

    const auto key_bytes = std::span<const std::uint8_t, kCredentialSize>{
        mapping, kCredentialSize};
    if (!std::ranges::equal(key_bytes.first<4>(), std::string_view{"S2UK"})
        || key_bytes[4] != 0
        || key_bytes[5] != 1
        || key_bytes[7] != 0) {
        return std::unexpected(KeyProviderError::kInvalidFormat);
    }
    const auto protection = key_bytes[6] == static_cast<std::uint8_t>(KeyProtection::kTpm2Bound)
        ? std::optional{KeyProtection::kTpm2Bound}
        : key_bytes[6] == static_cast<std::uint8_t>(KeyProtection::kHostKey)
            ? std::optional{KeyProtection::kHostKey}
            : std::nullopt;
    const auto version = read_u32_be(key_bytes.subspan<8, 4>());
    if (!protection || version == 0) {
        return std::unexpected(KeyProviderError::kInvalidFormat);
    }
    // Sealed once fully loaded: the key is unreadable while idle. All
    // subsequent reads go through with_bytes() (temporary PROT_READ).
    (void)::mprotect(mapping, MasterKey::kSealLength, PROT_NONE);
    return MasterKey{std::move(page), *protection, version};
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
