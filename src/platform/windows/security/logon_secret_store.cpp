#include "logon_secret_store.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace su::windows::security {
namespace {

constexpr wchar_t kSystemOnlySddl[] = L"D:P(A;;FA;;;SY)";

LogonSecretError map_status(SuStatus status) {
    switch (status) {
    case SuStatus_InvalidArgument:
    case SuStatus_InvalidUtf8:
    case SuStatus_NullArgument:
    case SuStatus_BufferTooSmall:
        return LogonSecretError::kInvalidArgument;
    case SuStatus_UserDenied:
        return LogonSecretError::kAccessDenied;
    case SuStatus_ParseError:
    case SuStatus_CryptoError:
    case SuStatus_MigrationRequired:
        return LogonSecretError::kCorrupt;
    case SuStatus_WriteError:
        return LogonSecretError::kWriteFailed;
    case SuStatus_IoError:
    case SuStatus_KeyUnavailable:
    case SuStatus_Ok:
        return LogonSecretError::kUnavailable;
    }
    return LogonSecretError::kUnavailable;
}

bool valid_sid_text(std::string_view sid) {
    return sid.size() >= 5 && sid.size() <= 184 && sid.starts_with("S-1-")
        && std::ranges::all_of(sid, [](char character) {
            return (character >= '0' && character <= '9')
                || character == '-' || character == 'S';
        });
}

std::expected<std::string, LogonSecretError> utf8_path(
    const std::filesystem::path& path) {
    const auto wide = path.wstring();
    const auto required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::unexpected(LogonSecretError::kInvalidArgument);
    }
    auto output = std::string(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
            output.data(), required, nullptr, nullptr) != required) {
        return std::unexpected(LogonSecretError::kInvalidArgument);
    }
    return output;
}

std::expected<void, LogonSecretError> create_system_directory(
    const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kSystemOnlySddl, SDDL_REVISION_1, &raw_descriptor, nullptr)) {
        return std::unexpected(LogonSecretError::kWriteFailed);
    }
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{
        raw_descriptor, &LocalFree};
    auto attributes = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor.get(),
        .bInheritHandle = FALSE,
    };
    if (!CreateDirectoryW(path.c_str(), &attributes)
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        return std::unexpected(LogonSecretError::kWriteFailed);
    }
    auto dacl_present = BOOL{};
    auto dacl_defaulted = BOOL{};
    auto* dacl = static_cast<PACL>(nullptr);
    if (!GetSecurityDescriptorDacl(
            descriptor.get(), &dacl_present, &dacl, &dacl_defaulted)
        || !dacl_present
        || SetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            dacl,
            nullptr) != ERROR_SUCCESS) {
        return std::unexpected(LogonSecretError::kWriteFailed);
    }
    const auto file_attributes = GetFileAttributesW(path.c_str());
    if (file_attributes == INVALID_FILE_ATTRIBUTES
        || (file_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        || (file_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return std::unexpected(LogonSecretError::kWriteFailed);
    }
    return {};
}

std::expected<std::filesystem::path, LogonSecretError> program_data_path() {
    PWSTR raw_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw_path))) {
        return std::unexpected(LogonSecretError::kUnavailable);
    }
    const auto path = std::filesystem::path{raw_path};
    CoTaskMemFree(raw_path);
    return path;
}

} // namespace

PreparedLogonSecret::PreparedLogonSecret(PreparedLogonSecret&& other) noexcept
    : password_(other.password_), size_(std::exchange(other.size_, 0)) {
    other.clear();
}

PreparedLogonSecret& PreparedLogonSecret::operator=(PreparedLogonSecret&& other) noexcept {
    if (this != &other) {
        clear();
        password_ = other.password_;
        size_ = std::exchange(other.size_, 0);
        other.clear();
    }
    return *this;
}

PreparedLogonSecret::~PreparedLogonSecret() {
    clear();
}

const wchar_t* PreparedLogonSecret::c_str() const {
    static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
    return reinterpret_cast<const wchar_t*>(password_.data());
}

void PreparedLogonSecret::clear() noexcept {
    SecureZeroMemory(password_.data(), password_.size() * sizeof(password_[0]));
    size_ = 0;
}

SuEncryptedStoreContext LogonSecretStore::context(const char* sid) const {
    return SuEncryptedStoreContext{
        .master_key = storage_key_.bytes().data(),
        .master_key_len = storage_key_.bytes().size(),
        .key_version = storage_key_.version(),
        .account_kind = static_cast<std::uint32_t>(SuAccountKind_WindowsSid),
        .linux_uid = 0,
        .windows_sid = sid,
    };
}

std::expected<std::filesystem::path, LogonSecretError> LogonSecretStore::secret_path(
    std::string_view sid) const {
    if (!valid_sid_text(sid) || sid.back() == '\0') {
        return std::unexpected(LogonSecretError::kInvalidArgument);
    }
    const auto root = program_data_path();
    if (!root) {
        return std::unexpected(root.error());
    }
    const auto product = *root / "Smile2Unlock";
    const auto users = product / "Users";
    const auto account = users / std::string{sid};
    for (const auto& directory : {product, users, account}) {
        if (const auto created = create_system_directory(directory); !created) {
            return std::unexpected(created.error());
        }
    }
    return account / "logon-secret.s2u";
}

std::expected<std::uint64_t, LogonSecretError> LogonSecretStore::store(
    std::string_view sid,
    std::string_view canonical_username,
    SuWindowsAccountKind account_kind,
    std::span<const wchar_t> password) const {
    const auto sid_string = std::string{sid};
    const auto path = secret_path(sid_string);
    const auto encoded_path = path ? utf8_path(*path) : std::expected<std::string, LogonSecretError>{
        std::unexpected(path.error())};
    if (!encoded_path || password.empty() || password.size() > 512) {
        return std::unexpected(
            encoded_path ? LogonSecretError::kInvalidArgument : encoded_path.error());
    }
    static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
    auto generation = std::uint64_t{0};
    const auto canonical_username_string = std::string{canonical_username};
    const auto ffi_context = context(sid_string.c_str());
    const auto status = su_core_store_windows_logon_secret(
        &ffi_context,
        encoded_path->c_str(),
        static_cast<std::uint32_t>(account_kind),
        canonical_username_string.c_str(),
        reinterpret_cast<const std::uint16_t*>(password.data()),
        password.size(),
        &generation);
    return status == SuStatus_Ok
        ? std::expected<std::uint64_t, LogonSecretError>{generation}
        : std::unexpected(map_status(status));
}

std::expected<PreparedLogonSecret, LogonSecretError> LogonSecretStore::prepare(
    std::string_view sid,
    std::uint64_t request_id,
    std::uint32_t logon_session_id) const {
    const auto sid_string = std::string{sid};
    const auto path = secret_path(sid_string);
    const auto encoded_path = path ? utf8_path(*path) : std::expected<std::string, LogonSecretError>{
        std::unexpected(path.error())};
    if (!encoded_path) {
        return std::unexpected(encoded_path.error());
    }
    auto password = PreparedLogonSecret{};
    auto password_len = std::uintptr_t{0};
    const auto ffi_context = context(sid_string.c_str());
    const auto status = su_core_prepare_windows_logon_secret(
        &ffi_context,
        encoded_path->c_str(),
        request_id,
        logon_session_id,
        password.password_.data(),
        password.password_.size(),
        &password_len);
    if (status != SuStatus_Ok) {
        return std::unexpected(map_status(status));
    }
    password.size_ = password_len;
    return password;
}

std::expected<void, LogonSecretError> LogonSecretStore::mark_stale(
    std::string_view sid) const {
    const auto sid_string = std::string{sid};
    const auto path = secret_path(sid_string);
    const auto encoded_path = path ? utf8_path(*path) : std::expected<std::string, LogonSecretError>{
        std::unexpected(path.error())};
    if (!encoded_path) {
        return std::unexpected(encoded_path.error());
    }
    const auto ffi_context = context(sid_string.c_str());
    const auto status = su_core_mark_windows_logon_secret_stale(
        &ffi_context, encoded_path->c_str());
    return status == SuStatus_Ok
        ? std::expected<void, LogonSecretError>{}
        : std::unexpected(map_status(status));
}

std::expected<bool, LogonSecretError> LogonSecretStore::clear(std::string_view sid) const {
    const auto sid_string = std::string{sid};
    const auto path = secret_path(sid_string);
    const auto encoded_path = path ? utf8_path(*path) : std::expected<std::string, LogonSecretError>{
        std::unexpected(path.error())};
    if (!encoded_path) {
        return std::unexpected(encoded_path.error());
    }
    auto cleared = false;
    const auto ffi_context = context(sid_string.c_str());
    const auto status = su_core_clear_windows_logon_secret(
        &ffi_context, encoded_path->c_str(), &cleared);
    return status == SuStatus_Ok
        ? std::expected<bool, LogonSecretError>{cleared}
        : std::unexpected(map_status(status));
}

std::string_view logon_secret_error_message(LogonSecretError error) {
    switch (error) {
    case LogonSecretError::kInvalidArgument: return "invalid logon secret input";
    case LogonSecretError::kUnavailable: return "logon secret unavailable";
    case LogonSecretError::kAccessDenied: return "logon secret is stale or already consumed";
    case LogonSecretError::kCorrupt: return "logon secret authentication failed";
    case LogonSecretError::kWriteFailed: return "logon secret write failed";
    }
    std::unreachable();
}

} // namespace su::windows::security
