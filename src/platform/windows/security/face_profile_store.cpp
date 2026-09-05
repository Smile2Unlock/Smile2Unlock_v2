#include "face_profile_store.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace su::windows::security {
namespace {

// Apply the SYSTEM-only ACE to descendants as well. Without OI/CI, files
// created below the directory could receive the process token's default ACL.
constexpr wchar_t kSystemOnlySddl[] = L"D:P(A;OICI;FA;;;SY)";

bool valid_sid_text(std::string_view sid) {
    return sid.size() >= 5 && sid.size() <= 184 && sid.starts_with("S-1-")
        && std::ranges::all_of(sid, [](char character) {
            return (character >= '0' && character <= '9')
                || character == '-' || character == 'S';
        });
}

FaceProfileStoreError map_status(SuStatus status) {
    switch (status) {
    case SuStatus_InvalidArgument:
    case SuStatus_InvalidUtf8:
    case SuStatus_NullArgument:
    case SuStatus_BufferTooSmall:
        return FaceProfileStoreError::kInvalidArgument;
    case SuStatus_ParseError:
    case SuStatus_CryptoError:
    case SuStatus_MigrationRequired:
        return FaceProfileStoreError::kCorrupt;
    case SuStatus_WriteError:
        return FaceProfileStoreError::kWriteFailed;
    case SuStatus_UserDenied:
        return FaceProfileStoreError::kNotFound;
    case SuStatus_IoError:
    case SuStatus_KeyUnavailable:
    case SuStatus_Ok:
        return FaceProfileStoreError::kUnavailable;
    }
    return FaceProfileStoreError::kUnavailable;
}

std::expected<std::string, FaceProfileStoreError> utf8_path(
    const std::filesystem::path& path) {
    const auto wide = path.wstring();
    const auto required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::unexpected(FaceProfileStoreError::kInvalidArgument);
    }
    auto output = std::string(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
            output.data(), required, nullptr, nullptr) != required) {
        return std::unexpected(FaceProfileStoreError::kInvalidArgument);
    }
    return output;
}

std::expected<void, FaceProfileStoreError> create_system_directory(
    const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kSystemOnlySddl, SDDL_REVISION_1, &raw_descriptor, nullptr)) {
        return std::unexpected(FaceProfileStoreError::kWriteFailed);
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
        return std::unexpected(FaceProfileStoreError::kWriteFailed);
    }
    auto dacl_present = BOOL{};
    auto dacl_defaulted = BOOL{};
    auto* dacl = static_cast<PACL>(nullptr);
    if (!GetSecurityDescriptorDacl(
            descriptor.get(), &dacl_present, &dacl, &dacl_defaulted)
        || !dacl_present
        || SetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, dacl, nullptr) != ERROR_SUCCESS) {
        return std::unexpected(FaceProfileStoreError::kWriteFailed);
    }
    const auto attributes_on_disk = GetFileAttributesW(path.c_str());
    if (attributes_on_disk == INVALID_FILE_ATTRIBUTES
        || (attributes_on_disk & FILE_ATTRIBUTE_DIRECTORY) == 0
        || (attributes_on_disk & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return std::unexpected(FaceProfileStoreError::kWriteFailed);
    }
    return {};
}

std::expected<std::filesystem::path, FaceProfileStoreError> program_data_path() {
    PWSTR raw_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw_path))) {
        return std::unexpected(FaceProfileStoreError::kUnavailable);
    }
    const auto path = std::filesystem::path{raw_path};
    CoTaskMemFree(raw_path);
    return path;
}

} // namespace

SuEncryptedStoreContext FaceProfileStore::context(const char* sid) const {
    return SuEncryptedStoreContext{
        .master_key = storage_key_.bytes().data(),
        .master_key_len = storage_key_.bytes().size(),
        .key_version = storage_key_.version(),
        .account_kind = static_cast<std::uint32_t>(SuAccountKind_WindowsSid),
        .linux_uid = 0,
        .windows_sid = sid,
    };
}

std::expected<std::filesystem::path, FaceProfileStoreError>
FaceProfileStore::profile_path(std::string_view sid) const {
    if (!valid_sid_text(sid) || sid.back() == '\0') {
        return std::unexpected(FaceProfileStoreError::kInvalidArgument);
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
    return account / "profiles.s2u";
}

std::expected<void, FaceProfileStoreError> FaceProfileStore::enroll(
    std::string_view sid,
    std::string_view label,
    std::string_view embedding_source) const {
    const auto sid_string = std::string{sid};
    const auto path = profile_path(sid);
    const auto encoded = path ? utf8_path(*path)
        : std::expected<std::string, FaceProfileStoreError>{std::unexpected(path.error())};
    if (!encoded || label.empty() || embedding_source.empty()) {
        return std::unexpected(encoded ? FaceProfileStoreError::kInvalidArgument : encoded.error());
    }
    const auto label_string = std::string{label};
    const auto source_string = std::string{embedding_source};
    const auto ffi_context = context(sid_string.c_str());
    const auto status = su_core_encrypted_enroll_face_profile(
        &ffi_context, encoded->c_str(), label_string.c_str(), source_string.c_str());
    return status == SuStatus_Ok
        ? std::expected<void, FaceProfileStoreError>{}
        : std::unexpected(map_status(status));
}

std::expected<bool, FaceProfileStoreError> FaceProfileStore::remove(
    std::string_view sid,
    std::string_view profile_id) const {
    const auto sid_string = std::string{sid};
    const auto path = profile_path(sid);
    const auto encoded = path ? utf8_path(*path)
        : std::expected<std::string, FaceProfileStoreError>{std::unexpected(path.error())};
    if (!encoded || profile_id.empty()) {
        return std::unexpected(encoded ? FaceProfileStoreError::kInvalidArgument : encoded.error());
    }
    auto deleted = false;
    const auto profile_id_string = std::string{profile_id};
    const auto ffi_context = context(sid_string.c_str());
    const auto status = su_core_encrypted_delete_face_profile(
        &ffi_context, encoded->c_str(), profile_id_string.c_str(), &deleted);
    return status == SuStatus_Ok
        ? std::expected<bool, FaceProfileStoreError>{deleted}
        : std::unexpected(map_status(status));
}

std::expected<std::string, FaceProfileStoreError> FaceProfileStore::list_json(
    std::string_view sid) const {
    const auto sid_string = std::string{sid};
    const auto path = profile_path(sid);
    const auto encoded = path ? utf8_path(*path)
        : std::expected<std::string, FaceProfileStoreError>{std::unexpected(path.error())};
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    const auto ffi_context = context(sid_string.c_str());
    auto required = std::uintptr_t{0};
    auto status = su_core_encrypted_list_face_profiles_json(
        &ffi_context, encoded->c_str(), nullptr, 0, &required);
    if (status == SuStatus_IoError) {
        return std::string{"[]"};
    }
    if (status != SuStatus_BufferTooSmall && status != SuStatus_Ok) {
        return std::unexpected(map_status(status));
    }
    if (required == 0) {
        return std::string{"[]"};
    }
    auto buffer = std::vector<std::uint8_t>(required);
    status = su_core_encrypted_list_face_profiles_json(
        &ffi_context, encoded->c_str(), buffer.data(), buffer.size(), &required);
    if (status != SuStatus_Ok || required > buffer.size()) {
        return std::unexpected(status == SuStatus_Ok
            ? FaceProfileStoreError::kCorrupt : map_status(status));
    }
    return std::string{reinterpret_cast<const char*>(buffer.data())};
}

std::expected<SuFaceAuthReport, FaceProfileStoreError> FaceProfileStore::authenticate(
    std::string_view sid,
    std::string_view embedding_source,
    float threshold,
    bool liveness_ok) const {
    const auto sid_string = std::string{sid};
    const auto path = profile_path(sid);
    const auto encoded = path ? utf8_path(*path)
        : std::expected<std::string, FaceProfileStoreError>{std::unexpected(path.error())};
    if (!encoded || embedding_source.empty()) {
        return std::unexpected(encoded ? FaceProfileStoreError::kInvalidArgument : encoded.error());
    }
    const auto source_string = std::string{embedding_source};
    const auto ffi_context = context(sid_string.c_str());
    auto report = SuFaceAuthReport{};
    const auto status = su_core_encrypted_authenticate_face_sample_report_into(
        &ffi_context, encoded->c_str(), source_string.c_str(), threshold, liveness_ok,
        &report);
    return status == SuStatus_Ok && report.status == SuStatus_Ok
        ? std::expected<SuFaceAuthReport, FaceProfileStoreError>{report}
        : std::unexpected(map_status(status != SuStatus_Ok ? status : report.status));
}

} // namespace su::windows::security
