#include "storage_key_provider.h"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <ncrypt.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <format>
#include <limits>
#include <utility>
#include <vector>

namespace su::windows::security {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'S', '2', 'W', 'K'};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::uint32_t kKeyVersion = 1;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kMaximumWrappedSize = 16 * 1024;
constexpr wchar_t kTpmKeyName[] = L"Smile2Unlock.StorageWrappingKey.v1";
constexpr wchar_t kSystemOnlySddl[] = L"D:P(A;OICI;FA;;;SY)";

template <typename Handle, auto Close>
class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(Handle handle) : handle_(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, Handle{})) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, Handle{});
        }
        return *this;
    }
    ~ScopedHandle() { reset(); }
    [[nodiscard]] Handle get() const { return handle_; }
    [[nodiscard]] explicit operator bool() const { return handle_ != Handle{}; }
    void reset() {
        if (handle_ != Handle{}) {
            (void)Close(handle_);
            handle_ = Handle{};
        }
    }

private:
    Handle handle_{};
};

using ScopedNcryptProvider = ScopedHandle<NCRYPT_PROV_HANDLE, NCryptFreeObject>;
using ScopedNcryptKey = ScopedHandle<NCRYPT_KEY_HANDLE, NCryptFreeObject>;
using ScopedWinHandle = ScopedHandle<HANDLE, CloseHandle>;

struct LocalMemoryDeleter {
    void operator()(void* pointer) const noexcept {
        if (pointer != nullptr) {
            LocalFree(pointer);
        }
    }
};

using LocalMemory = std::unique_ptr<void, LocalMemoryDeleter>;

void clear_key(std::array<std::uint8_t, 32>* key) {
    if (key != nullptr) {
        SecureZeroMemory(key->data(), key->size());
        (void)VirtualUnlock(key->data(), key->size());
    }
}

std::uint32_t read_u32_be(std::span<const std::uint8_t, 4> bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24)
        | (static_cast<std::uint32_t>(bytes[1]) << 16)
        | (static_cast<std::uint32_t>(bytes[2]) << 8)
        | static_cast<std::uint32_t>(bytes[3]);
}

void append_u32_be(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

std::expected<LocalMemory, StorageKeyError> system_only_descriptor() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kSystemOnlySddl,
            SDDL_REVISION_1,
            &descriptor,
            nullptr)) {
        return std::unexpected(StorageKeyError::kWriteFailed);
    }
    return LocalMemory{descriptor};
}

std::expected<void, StorageKeyError> apply_system_only_file_acl(
    const std::filesystem::path& path) {
    const auto descriptor = system_only_descriptor();
    if (!descriptor) {
        return std::unexpected(descriptor.error());
    }
    auto dacl_present = BOOL{};
    auto dacl_defaulted = BOOL{};
    auto* dacl = static_cast<PACL>(nullptr);
    if (!GetSecurityDescriptorDacl(
            descriptor->get(), &dacl_present, &dacl, &dacl_defaulted)
        || !dacl_present) {
        return std::unexpected(StorageKeyError::kUnsafePath);
    }
    // SetNamedSecurityInfoW (path-based) instead of SetSecurityInfo (handle
    // based): the handle-based call fails with ERROR_ACCESS_DENIED on some
    // systems even when the handle was opened with WRITE_DAC (observed on a
    // Windows 10 19044 VM with every handle flag combination), while the
    // path-based API used by icacls/Set-Acl works. Same DACL semantics.
    auto wide_path = path.wstring();
    const auto status = SetNamedSecurityInfoW(
        wide_path.data(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        dacl,
        nullptr);
    if (status != ERROR_SUCCESS) {
        return std::unexpected(StorageKeyError::kUnsafePath);
    }
    return {};
}

std::expected<void, StorageKeyError> ensure_parent_directory(
    const std::filesystem::path& path) {
    const auto descriptor = system_only_descriptor();
    if (!descriptor) {
        return std::unexpected(descriptor.error());
    }
    auto attributes = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor->get(),
        .bInheritHandle = FALSE,
    };
    const auto parent = path.parent_path();
    std::error_code error;
    if (!std::filesystem::exists(parent, error)
        && !CreateDirectoryW(parent.c_str(), &attributes)
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        return std::unexpected(StorageKeyError::kWriteFailed);
    }
    if (error) {
        return std::unexpected(StorageKeyError::kUnsafePath);
    }
    const auto directory = ScopedWinHandle{CreateFileW(
        parent.c_str(),
        FILE_READ_ATTRIBUTES | WRITE_DAC,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    if (!directory || directory.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(StorageKeyError::kUnsafePath);
    }
    auto information = BY_HANDLE_FILE_INFORMATION{};
    if (!GetFileInformationByHandle(directory.get(), &information)
        || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return std::unexpected(StorageKeyError::kUnsafePath);
    }
    return apply_system_only_file_acl(parent);
}

std::expected<std::vector<std::uint8_t>, StorageKeyError> read_wrapped_file(
    const std::filesystem::path& path) {
    const auto file = ScopedWinHandle{CreateFileW(
        path.c_str(),
        GENERIC_READ | WRITE_DAC,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    if (!file || file.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(StorageKeyError::kUnavailable);
    }
    if (const auto secured = apply_system_only_file_acl(path); !secured) {
        return std::unexpected(secured.error());
    }
    auto information = BY_HANDLE_FILE_INFORMATION{};
    if (!GetFileInformationByHandle(file.get(), &information)
        || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || information.nFileSizeHigh != 0
        || information.nFileSizeLow < kHeaderSize
        || information.nFileSizeLow > kMaximumWrappedSize) {
        return std::unexpected(StorageKeyError::kUnsafePath);
    }
    auto bytes = std::vector<std::uint8_t>(information.nFileSizeLow);
    auto read = DWORD{0};
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
        || read != bytes.size()) {
        return std::unexpected(StorageKeyError::kUnavailable);
    }
    return bytes;
}

std::expected<void, StorageKeyError> atomic_write_system_only(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes) {
    if (const auto parent = ensure_parent_directory(path); !parent) {
        return parent;
    }
    const auto descriptor = system_only_descriptor();
    if (!descriptor) {
        return std::unexpected(descriptor.error());
    }
    auto attributes = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor->get(),
        .bInheritHandle = FALSE,
    };
    const auto temporary = path.wstring() + std::format(L".new-{}", GetCurrentProcessId());
    auto file = ScopedWinHandle{CreateFileW(
        temporary.c_str(),
        GENERIC_WRITE,
        0,
        &attributes,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr)};
    if (!file || file.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(StorageKeyError::kWriteFailed);
    }
    auto written = DWORD{0};
    if (bytes.size() > std::numeric_limits<DWORD>::max()
        || !WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        || written != bytes.size()
        || !FlushFileBuffers(file.get())) {
        DeleteFileW(temporary.c_str());
        return std::unexpected(StorageKeyError::kWriteFailed);
    }
    file.reset();
    if (!MoveFileExW(
            temporary.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return std::unexpected(StorageKeyError::kWriteFailed);
    }
    return {};
}

bool platform_provider_missing(SECURITY_STATUS status) {
    if (status == NTE_BAD_PROVIDER || status == NTE_PROVIDER_DLL_FAIL) {
        return true;
    }
#ifdef NTE_DEVICE_NOT_FOUND
    if (status == NTE_DEVICE_NOT_FOUND) {
        return true;
    }
#endif
    return false;
}

std::expected<ScopedNcryptProvider, StorageKeyError> open_tpm_provider() {
    auto provider = NCRYPT_PROV_HANDLE{};
    const auto status = NCryptOpenStorageProvider(
        &provider, MS_PLATFORM_CRYPTO_PROVIDER, 0);
    if (status == ERROR_SUCCESS) {
        return ScopedNcryptProvider{provider};
    }
    return std::unexpected(
        platform_provider_missing(status)
            ? StorageKeyError::kUnavailable
            : StorageKeyError::kTpmFailed);
}

std::expected<ScopedNcryptKey, StorageKeyError> open_or_create_tpm_key(
    NCRYPT_PROV_HANDLE provider) {
    const auto apply_key_acl = [](NCRYPT_KEY_HANDLE key)
        -> std::expected<void, StorageKeyError> {
        const auto descriptor = system_only_descriptor();
        if (!descriptor
            || NCryptSetProperty(
                key,
                NCRYPT_SECURITY_DESCR_PROPERTY,
                static_cast<PBYTE>(descriptor->get()),
                GetSecurityDescriptorLength(descriptor->get()),
                DACL_SECURITY_INFORMATION) != ERROR_SUCCESS) {
            return std::unexpected(StorageKeyError::kTpmFailed);
        }
        return {};
    };
    auto key = NCRYPT_KEY_HANDLE{};
    auto status = NCryptOpenKey(provider, &key, kTpmKeyName, 0, NCRYPT_MACHINE_KEY_FLAG);
    if (status == ERROR_SUCCESS) {
        auto owned_key = ScopedNcryptKey{key};
        if (const auto secured = apply_key_acl(key); !secured) {
            return std::unexpected(secured.error());
        }
        return owned_key;
    }
    status = NCryptCreatePersistedKey(
        provider,
        &key,
        NCRYPT_RSA_ALGORITHM,
        kTpmKeyName,
        AT_KEYEXCHANGE,
        NCRYPT_MACHINE_KEY_FLAG);
    if (status != ERROR_SUCCESS) {
        return std::unexpected(StorageKeyError::kTpmFailed);
    }
    auto owned_key = ScopedNcryptKey{key};
    auto length = DWORD{2048};
    auto export_policy = DWORD{0};
    if (NCryptSetProperty(
            key, NCRYPT_LENGTH_PROPERTY,
            reinterpret_cast<PBYTE>(&length), sizeof(length), 0) != ERROR_SUCCESS
        || NCryptSetProperty(
            key, NCRYPT_EXPORT_POLICY_PROPERTY,
            reinterpret_cast<PBYTE>(&export_policy), sizeof(export_policy), 0) != ERROR_SUCCESS
        || NCryptFinalizeKey(key, NCRYPT_MACHINE_KEY_FLAG) != ERROR_SUCCESS) {
        return std::unexpected(StorageKeyError::kTpmFailed);
    }
    if (const auto secured = apply_key_acl(key); !secured) {
        return std::unexpected(secured.error());
    }
    return owned_key;
}

std::expected<std::vector<std::uint8_t>, StorageKeyError> wrap_with_tpm(
    std::span<const std::uint8_t, 32> master_key) {
    auto provider = open_tpm_provider();
    if (!provider) {
        return std::unexpected(provider.error());
    }
    auto key = open_or_create_tpm_key(provider->get());
    if (!key) {
        return std::unexpected(key.error());
    }
    auto padding = BCRYPT_OAEP_PADDING_INFO{
        .pszAlgId = const_cast<LPWSTR>(BCRYPT_SHA256_ALGORITHM),
        .pbLabel = nullptr,
        .cbLabel = 0,
    };
    auto required = DWORD{0};
    if (NCryptEncrypt(
            key->get(),
            const_cast<BYTE*>(master_key.data()),
            static_cast<DWORD>(master_key.size()),
            &padding,
            nullptr,
            0,
            &required,
            NCRYPT_PAD_OAEP_FLAG) != ERROR_SUCCESS) {
        return std::unexpected(StorageKeyError::kTpmFailed);
    }
    auto wrapped = std::vector<std::uint8_t>(required);
    if (NCryptEncrypt(
            key->get(),
            const_cast<BYTE*>(master_key.data()),
            static_cast<DWORD>(master_key.size()),
            &padding,
            wrapped.data(),
            required,
            &required,
            NCRYPT_PAD_OAEP_FLAG) != ERROR_SUCCESS) {
        return std::unexpected(StorageKeyError::kTpmFailed);
    }
    wrapped.resize(required);
    return wrapped;
}

std::expected<std::array<std::uint8_t, 32>, StorageKeyError> unwrap_with_tpm(
    std::span<const std::uint8_t> wrapped) {
    auto provider = open_tpm_provider();
    if (!provider) {
        return std::unexpected(StorageKeyError::kTpmFailed);
    }
    auto key_handle = NCRYPT_KEY_HANDLE{};
    if (NCryptOpenKey(
            provider->get(), &key_handle, kTpmKeyName, 0, NCRYPT_MACHINE_KEY_FLAG)
        != ERROR_SUCCESS) {
        return std::unexpected(StorageKeyError::kTpmFailed);
    }
    auto key = ScopedNcryptKey{key_handle};
    auto padding = BCRYPT_OAEP_PADDING_INFO{
        .pszAlgId = const_cast<LPWSTR>(BCRYPT_SHA256_ALGORITHM),
        .pbLabel = nullptr,
        .cbLabel = 0,
    };
    auto master_key = std::array<std::uint8_t, 32>{};
    auto written = DWORD{0};
    if (wrapped.size() > std::numeric_limits<DWORD>::max()
        || NCryptDecrypt(
            key.get(),
            const_cast<BYTE*>(wrapped.data()),
            static_cast<DWORD>(wrapped.size()),
            &padding,
            master_key.data(),
            static_cast<DWORD>(master_key.size()),
            &written,
            NCRYPT_PAD_OAEP_FLAG) != ERROR_SUCCESS
        || written != master_key.size()) {
        SecureZeroMemory(master_key.data(), master_key.size());
        return std::unexpected(StorageKeyError::kTpmFailed);
    }
    return master_key;
}

std::expected<std::vector<std::uint8_t>, StorageKeyError> wrap_with_dpapi(
    std::span<const std::uint8_t, 32> master_key) {
    auto input = DATA_BLOB{
        .cbData = static_cast<DWORD>(master_key.size()),
        .pbData = const_cast<BYTE*>(master_key.data()),
    };
    auto output = DATA_BLOB{};
    if (!CryptProtectData(
            &input,
            L"Smile2Unlock storage master key",
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
            &output)) {
        return std::unexpected(StorageKeyError::kDpapiFailed);
    }
    const auto clear_output = LocalMemory{output.pbData};
    return std::vector<std::uint8_t>(output.pbData, output.pbData + output.cbData);
}

std::expected<std::array<std::uint8_t, 32>, StorageKeyError> unwrap_with_dpapi(
    std::span<const std::uint8_t> wrapped) {
    if (wrapped.size() > std::numeric_limits<DWORD>::max()) {
        return std::unexpected(StorageKeyError::kInvalidFormat);
    }
    auto input = DATA_BLOB{
        .cbData = static_cast<DWORD>(wrapped.size()),
        .pbData = const_cast<BYTE*>(wrapped.data()),
    };
    auto output = DATA_BLOB{};
    if (!CryptUnprotectData(
            &input,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &output)) {
        return std::unexpected(StorageKeyError::kDpapiFailed);
    }
    const auto clear_output = LocalMemory{output.pbData};
    if (output.cbData != 32) {
        SecureZeroMemory(output.pbData, output.cbData);
        return std::unexpected(StorageKeyError::kInvalidFormat);
    }
    auto master_key = std::array<std::uint8_t, 32>{};
    std::ranges::copy_n(output.pbData, master_key.size(), master_key.begin());
    SecureZeroMemory(output.pbData, output.cbData);
    return master_key;
}

std::vector<std::uint8_t> serialize_wrapped_key(
    KeyProtection protection,
    std::span<const std::uint8_t> wrapped) {
    auto output = std::vector<std::uint8_t>{};
    output.reserve(kHeaderSize + wrapped.size());
    output.insert(output.end(), kMagic.begin(), kMagic.end());
    output.push_back(0);
    output.push_back(static_cast<std::uint8_t>(kFormatVersion));
    output.push_back(static_cast<std::uint8_t>(protection));
    output.push_back(0);
    append_u32_be(output, kKeyVersion);
    append_u32_be(output, static_cast<std::uint32_t>(wrapped.size()));
    output.insert(output.end(), wrapped.begin(), wrapped.end());
    return output;
}

} // namespace

StorageKey::StorageKey(
    std::unique_ptr<std::array<std::uint8_t, 32>> bytes,
    std::uint32_t version,
    KeyProtection protection)
    : bytes_(std::move(bytes)), version_(version), protection_(protection) {}

StorageKey::StorageKey(StorageKey&& other) noexcept = default;
StorageKey& StorageKey::operator=(StorageKey&& other) noexcept {
    if (this != &other) {
        clear_key(bytes_.get());
        bytes_ = std::move(other.bytes_);
        version_ = std::exchange(other.version_, 0);
        protection_ = other.protection_;
    }
    return *this;
}

StorageKey::~StorageKey() {
    clear_key(bytes_.get());
}

std::span<const std::uint8_t, 32> StorageKey::bytes() const {
    return *bytes_;
}

std::expected<StorageKey, StorageKeyError> load_or_create_storage_key(
    const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        const auto serialized = read_wrapped_file(path);
        if (!serialized || serialized->size() < kHeaderSize
            || !std::equal(serialized->begin(), serialized->begin() + 4, kMagic.begin())
            || (*serialized)[4] != 0 || (*serialized)[5] != kFormatVersion
            || (*serialized)[7] != 0) {
            return std::unexpected(
                serialized ? StorageKeyError::kInvalidFormat : serialized.error());
        }
        const auto protection = static_cast<KeyProtection>((*serialized)[6]);
        const auto key_version = read_u32_be(std::span<const std::uint8_t, 4>{
            serialized->data() + 8, 4});
        const auto payload_size = read_u32_be(std::span<const std::uint8_t, 4>{
            serialized->data() + 12, 4});
        if (key_version == 0 || payload_size != serialized->size() - kHeaderSize) {
            return std::unexpected(StorageKeyError::kInvalidFormat);
        }
        const auto payload = std::span<const std::uint8_t>{*serialized}.subspan(kHeaderSize);
        auto unwrapped = protection == KeyProtection::kTpm2Bound
            ? unwrap_with_tpm(payload)
            : protection == KeyProtection::kMachineDpapi
                ? unwrap_with_dpapi(payload)
                : std::expected<std::array<std::uint8_t, 32>, StorageKeyError>{
                    std::unexpected(StorageKeyError::kInvalidFormat)};
        if (!unwrapped) {
            return std::unexpected(unwrapped.error());
        }
        auto key = std::make_unique<std::array<std::uint8_t, 32>>(*unwrapped);
        SecureZeroMemory(unwrapped->data(), unwrapped->size());
        if (!VirtualLock(key->data(), key->size())) {
            clear_key(key.get());
            return std::unexpected(StorageKeyError::kMemoryLockFailed);
        }
        return StorageKey{std::move(key), key_version, protection};
    }
    if (error) {
        return std::unexpected(StorageKeyError::kUnsafePath);
    }

    auto key = std::make_unique<std::array<std::uint8_t, 32>>();
    if (!BCRYPT_SUCCESS(BCryptGenRandom(
            nullptr, key->data(), static_cast<ULONG>(key->size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return std::unexpected(StorageKeyError::kRandomFailed);
    }
    if (!VirtualLock(key->data(), key->size())) {
        clear_key(key.get());
        return std::unexpected(StorageKeyError::kMemoryLockFailed);
    }

    auto protection = KeyProtection::kTpm2Bound;
    auto wrapped = wrap_with_tpm(*key);
    // TPM absence or failure must fall back to machine DPAPI. wrap_with_tpm
    // surfaces kTpmFailed for most NCrypt failures (not just kUnavailable),
    // so accept both here; otherwise machines without a TPM (VMs) can never
    // create a storage key.
    if (!wrapped && (wrapped.error() == StorageKeyError::kUnavailable
                     || wrapped.error() == StorageKeyError::kTpmFailed)) {
        protection = KeyProtection::kMachineDpapi;
        wrapped = wrap_with_dpapi(*key);
    }
    if (!wrapped) {
        clear_key(key.get());
        return std::unexpected(wrapped.error());
    }
    const auto serialized = serialize_wrapped_key(protection, *wrapped);
    if (const auto written = atomic_write_system_only(path, serialized); !written) {
        clear_key(key.get());
        return std::unexpected(written.error());
    }
    return StorageKey{std::move(key), kKeyVersion, protection};
}

std::filesystem::path default_storage_key_path() {
    PWSTR program_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &program_data))) {
        return {};
    }
    const auto free_path = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>{
        program_data, &CoTaskMemFree};
    return std::filesystem::path{program_data} / "Smile2Unlock" / "storage-master.s2k";
}

std::string_view key_protection_name(KeyProtection protection) {
    switch (protection) {
    case KeyProtection::kTpm2Bound: return "TPM2-bound";
    case KeyProtection::kMachineDpapi: return "machine-dpapi";
    }
    std::unreachable();
}

std::string_view storage_key_error_message(StorageKeyError error) {
    switch (error) {
    case StorageKeyError::kUnavailable: return "key provider unavailable";
    case StorageKeyError::kUnsafePath: return "unsafe key path";
    case StorageKeyError::kInvalidFormat: return "invalid wrapped key format";
    case StorageKeyError::kRandomFailed: return "random generation failed";
    case StorageKeyError::kTpmFailed: return "TPM key operation failed";
    case StorageKeyError::kDpapiFailed: return "machine DPAPI operation failed";
    case StorageKeyError::kWriteFailed: return "wrapped key write failed";
    case StorageKeyError::kMemoryLockFailed: return "master key memory lock failed";
    }
    std::unreachable();
}

} // namespace su::windows::security
