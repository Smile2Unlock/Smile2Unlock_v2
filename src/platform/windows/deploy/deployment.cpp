// Plain (non-module) TU: winreg.h etc. are safe to include here.

#include "deployment.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsvc.h>
#include <shlobj.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace su::windeploy {

namespace {

constexpr wchar_t kClsid[] = L"{5fd3d285-0dd9-4362-8855-e0abaacd4af6}";
constexpr wchar_t kClsidKeyPath[] =
    L"SOFTWARE\\Classes\\CLSID\\{5fd3d285-0dd9-4362-8855-e0abaacd4af6}";
constexpr wchar_t kCredentialProvidersKeyPath[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\CredentialProviders";
constexpr wchar_t kCredentialProviderKeyPath[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\CredentialProviders\\"
    L"{5fd3d285-0dd9-4362-8855-e0abaacd4af6}";
constexpr wchar_t kServiceName[] = L"Smile2UnlockAuthService";

struct InstalledComponents {
    std::filesystem::path credential_provider;
    std::filesystem::path auth_service;
    std::filesystem::path recognition_agent;
};

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~UniqueHandle() {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            ::CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
                ::CloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const { return handle_; }
    [[nodiscard]] explicit operator bool() const {
        return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }
private:
    HANDLE handle_;
};

struct PackageManifest {
    std::filesystem::path root;
    std::unordered_map<std::string, std::string> hashes;
};

std::string wide_to_utf8(std::wstring_view text);

std::string win32_error(std::string_view action, DWORD error = ::GetLastError()) {
    return std::format("{} (Win32 error {})", action, error);
}

std::expected<UniqueHandle, std::string> open_plain_file(
    const std::filesystem::path& path) {
    auto handle = UniqueHandle{::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr)};
    if (!handle) {
        return std::unexpected(win32_error("failed to open " + path.filename().string()));
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!::GetFileInformationByHandleEx(
            handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        return std::unexpected(win32_error("failed to inspect " + path.filename().string()));
    }
    if ((attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return std::unexpected("deployment input must be a regular non-reparse file: "
            + path.string());
    }
    return handle;
}

std::expected<void, std::string> ensure_plain_directory(const std::filesystem::path& path) {
    auto handle = UniqueHandle{::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!handle) {
        return std::unexpected(win32_error("failed to open protected install directory"));
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!::GetFileInformationByHandleEx(
            handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        return std::unexpected(win32_error("failed to inspect protected install directory"));
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return std::unexpected("protected install path is not a plain directory: " + path.string());
    }
    return {};
}

std::expected<std::string, std::string> read_all(HANDLE file, std::size_t limit) {
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
        return std::unexpected(win32_error("failed to rewind deployment input"));
    }
    auto content = std::string{};
    auto buffer = std::array<char, 16 * 1024>{};
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            return std::unexpected(win32_error("failed to read deployment input"));
        }
        if (read == 0) {
            break;
        }
        if (content.size() + read > limit) {
            return std::unexpected("deployment input exceeds its size limit");
        }
        content.append(buffer.data(), read);
    }
    return content;
}

std::expected<void, std::string> verify_authenticode(
    HANDLE file, const std::filesystem::path& display_path) {
    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = display_path.c_str();
    file_info.hFile = file;

    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file_info;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto status = ::WinVerifyTrust(nullptr, &policy, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)::WinVerifyTrust(nullptr, &policy, &trust);
    if (status != ERROR_SUCCESS) {
        return std::unexpected(std::format(
            "Authenticode verification failed for {} (status 0x{:08x})",
            display_path.filename().string(), static_cast<std::uint32_t>(status)));
    }
    return {};
}

std::expected<void, std::string> verify_detached_manifest_signature(
    HANDLE manifest, HANDLE signature) {
    const auto content = read_all(manifest, 4 * 1024 * 1024);
    if (!content) {
        return std::unexpected(content.error());
    }
    const auto signature_bytes = read_all(signature, 1024 * 1024);
    if (!signature_bytes) {
        return std::unexpected(signature_bytes.error());
    }
    CRYPT_VERIFY_MESSAGE_PARA parameters{};
    parameters.cbSize = sizeof(parameters);
    parameters.dwMsgAndCertEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    parameters.hCryptProv = 0;
    parameters.pfnGetSignerCertificate = nullptr;
    parameters.pvGetArg = nullptr;
    const BYTE* content_parts[] = {
        reinterpret_cast<const BYTE*>(content->data()),
    };
    DWORD content_sizes[] = {static_cast<DWORD>(content->size())};
    PCCERT_CONTEXT signer = nullptr;
    if (!::CryptVerifyDetachedMessageSignature(
            &parameters, 0,
            reinterpret_cast<const BYTE*>(signature_bytes->data()),
            static_cast<DWORD>(signature_bytes->size()), 1, content_parts, content_sizes,
            &signer)) {
        return std::unexpected(win32_error("release-info signature verification failed"));
    }
    const auto release_signer = std::unique_ptr<const CERT_CONTEXT, decltype(&::CertFreeCertificateContext)>{
        signer, &::CertFreeCertificateContext};
    CERT_CHAIN_PARA chain_parameters{};
    chain_parameters.cbSize = sizeof(chain_parameters);
    CERT_ENHKEY_USAGE usage{};
    LPSTR usages[] = {const_cast<LPSTR>(szOID_PKIX_KP_CODE_SIGNING)};
    usage.cUsageIdentifier = 1;
    usage.rgpszUsageIdentifier = usages;
    chain_parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    chain_parameters.RequestedUsage.Usage = usage;
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    if (!::CertGetCertificateChain(
            nullptr, signer, nullptr, signer->hCertStore, &chain_parameters,
            CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY, nullptr, &chain)) {
        return std::unexpected(win32_error("failed to build release signer trust chain"));
    }
    const auto release_chain = std::unique_ptr<const CERT_CHAIN_CONTEXT, decltype(&::CertFreeCertificateChain)>{
        chain, &::CertFreeCertificateChain};
    CERT_CHAIN_POLICY_PARA policy_parameters{};
    policy_parameters.cbSize = sizeof(policy_parameters);
    CERT_CHAIN_POLICY_STATUS policy_status{};
    policy_status.cbSize = sizeof(policy_status);
    if (!::CertVerifyCertificateChainPolicy(
            CERT_CHAIN_POLICY_AUTHENTICODE, chain, &policy_parameters, &policy_status)
        || policy_status.dwError != ERROR_SUCCESS) {
        return std::unexpected(std::format(
            "release signer is not trusted for code signing (status 0x{:08x})",
            static_cast<std::uint32_t>(policy_status.dwError)));
    }
    return {};
}

std::expected<PackageManifest, std::string> load_package_manifest(
    const std::filesystem::path& source_bin) {
    if (source_bin.filename() != L"bin") {
        return std::unexpected("deployment source must use the signed package bin directory");
    }
    const auto root = source_bin.parent_path();
    auto manifest = open_plain_file(root / "release-info.json");
    if (!manifest) {
        return std::unexpected(manifest.error());
    }
    auto signature = open_plain_file(root / "release-info.p7s");
    if (!signature) {
        return std::unexpected(signature.error());
    }
    if (const auto verified = verify_detached_manifest_signature(
            manifest->get(), signature->get()); !verified) {
        return std::unexpected(verified.error());
    }
    const auto text = read_all(manifest->get(), 4 * 1024 * 1024);
    if (!text) {
        return std::unexpected(text.error());
    }
    const auto parsed = nlohmann::json::parse(*text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()
        || parsed.value("schema", 0) != 1
        || !parsed.contains("build") || !parsed["build"].is_object()
        || parsed["build"].value("platform", "") != "windows"
        || !parsed.contains("files") || !parsed["files"].is_array()) {
        return std::unexpected("release-info.json has an invalid Windows package schema");
    }
    auto result = PackageManifest{.root = root, .hashes = {}};
    for (const auto& entry : parsed["files"]) {
        if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string()
            || !entry.contains("sha256") || !entry["sha256"].is_string()) {
            return std::unexpected("release-info.json contains an invalid file entry");
        }
        const auto relative = entry["path"].get<std::string>();
        const auto digest = entry["sha256"].get<std::string>();
        const auto relative_path = std::filesystem::path{relative};
        if (relative.empty() || relative_path.is_absolute()
            || std::ranges::any_of(relative_path, [](const auto& component) {
                return component == std::filesystem::path{".."};
            })
            || digest.size() != 64
            || !std::ranges::all_of(digest, [](unsigned char ch) { return std::isxdigit(ch); })
            || !result.hashes.emplace(relative, digest).second) {
            return std::unexpected("release-info.json contains an unsafe or duplicate path");
        }
    }
    return result;
}

std::expected<std::filesystem::path, std::string> program_files_root() {
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &raw))) {
        return std::unexpected("failed to resolve Program Files");
    }
    const auto path = std::filesystem::path{raw};
    ::CoTaskMemFree(raw);
    return path;
}

std::string hex_digest(const BYTE* bytes, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    auto result = std::string(size * 2, '0');
    for (std::size_t index = 0; index < size; ++index) {
        result[index * 2] = digits[bytes[index] >> 4];
        result[index * 2 + 1] = digits[bytes[index] & 0x0f];
    }
    return result;
}

std::expected<void, std::string> copy_required_file(
    const PackageManifest& package,
    std::string_view relative,
    const std::filesystem::path& destination,
    bool executable) {
    const auto expected = package.hashes.find(std::string(relative));
    if (expected == package.hashes.end()) {
        return std::unexpected("required deployment file is absent from signed release-info: "
            + std::string(relative));
    }
    const auto source_path = package.root / std::filesystem::path{relative};
    auto source = open_plain_file(source_path);
    if (!source) {
        return std::unexpected(source.error());
    }
    if (executable) {
        if (const auto verified = verify_authenticode(source->get(), source_path); !verified) {
            return std::unexpected(verified.error());
        }
    }
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(source->get(), zero, nullptr, FILE_BEGIN)) {
        return std::unexpected(win32_error("failed to rewind signed deployment file"));
    }
    const auto destination_attributes = ::GetFileAttributesW(destination.c_str());
    if (destination_attributes != INVALID_FILE_ATTRIBUTES
        && (destination_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return std::unexpected("refusing to replace a reparse-point destination: "
            + destination.string());
    }
    auto temp = destination;
    temp += std::format(L".install-{:08x}-{:08x}",
        ::GetCurrentProcessId(), ::GetTickCount());
    auto output = UniqueHandle{::CreateFileW(
        temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY, nullptr)};
    if (!output) {
        return std::unexpected(win32_error("failed to create protected temporary file"));
    }

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!::CryptAcquireContextW(
            &provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)
        || !::CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        if (provider != 0) {
            ::CryptReleaseContext(provider, 0);
        }
        output = UniqueHandle{};
        (void)::DeleteFileW(temp.c_str());
        return std::unexpected(win32_error("failed to initialize SHA-256"));
    }
    const auto cleanup_crypto = [&] {
        if (hash != 0) {
            ::CryptDestroyHash(hash);
        }
        if (provider != 0) {
            ::CryptReleaseContext(provider, 0);
        }
    };
    auto buffer = std::array<BYTE, 64 * 1024>{};
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(source->get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            cleanup_crypto();
            output = UniqueHandle{};
            (void)::DeleteFileW(temp.c_str());
            return std::unexpected(win32_error("failed to read signed deployment file"));
        }
        if (read == 0) {
            break;
        }
        DWORD written = 0;
        if (!::CryptHashData(hash, buffer.data(), read, 0)
            || !::WriteFile(output.get(), buffer.data(), read, &written, nullptr)
            || written != read) {
            cleanup_crypto();
            output = UniqueHandle{};
            (void)::DeleteFileW(temp.c_str());
            return std::unexpected(win32_error("failed while copying signed deployment file"));
        }
    }
    auto digest = std::array<BYTE, 32>{};
    DWORD digest_size = static_cast<DWORD>(digest.size());
    if (!::CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digest_size, 0)) {
        cleanup_crypto();
        output = UniqueHandle{};
        (void)::DeleteFileW(temp.c_str());
        return std::unexpected(win32_error("failed to finish deployment SHA-256"));
    }
    cleanup_crypto();
    auto expected_digest = expected->second;
    std::ranges::transform(expected_digest, expected_digest.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (hex_digest(digest.data(), digest_size) != expected_digest) {
        output = UniqueHandle{};
        (void)::DeleteFileW(temp.c_str());
        return std::unexpected("signed release-info hash mismatch for " + std::string(relative));
    }
    if (!::FlushFileBuffers(output.get())) {
        output = UniqueHandle{};
        (void)::DeleteFileW(temp.c_str());
        return std::unexpected(win32_error("failed to flush protected temporary file"));
    }
    output = UniqueHandle{};
    if (!::MoveFileExW(temp.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = win32_error("failed to atomically install "
            + destination.filename().string());
        (void)::DeleteFileW(temp.c_str());
        return std::unexpected(error);
    }
    return {};
}

std::expected<InstalledComponents, std::string> stage_security_components(
    const std::filesystem::path& source_bin) {
    const auto package = load_package_manifest(source_bin);
    if (!package) {
        return std::unexpected(package.error());
    }
    const auto program_files = program_files_root();
    if (!program_files) {
        return std::unexpected(program_files.error());
    }
    const auto install_root = *program_files / "Smile2Unlock";
    const auto install_bin = install_root / "bin";
    const auto install_models = install_root / "assets" / "models" / "seeta";
    const auto install_i18n = install_root / "assets" / "i18n";
    auto error = std::error_code{};
    std::filesystem::create_directories(install_bin, error);
    if (error) {
        return std::unexpected("failed to create the protected install directory: "
            + error.message());
    }
    std::filesystem::create_directories(install_models, error);
    if (error) {
        return std::unexpected("failed to create the protected model directory: "
            + error.message());
    }
    std::filesystem::create_directories(install_i18n, error);
    if (error) {
        return std::unexpected("failed to create the translation directory: "
            + error.message());
    }

    for (const auto& directory : {install_root, install_bin, install_root / "assets",
             install_root / "assets" / "models", install_models, install_i18n}) {
        if (const auto checked = ensure_plain_directory(directory); !checked) {
            return std::unexpected(checked.error());
        }
    }

    const auto installed = InstalledComponents{
        .credential_provider = install_bin / "su_credential_provider.dll",
        .auth_service = install_bin / "Smile2UnlockAuthService.exe",
        .recognition_agent = install_bin / "su_recognition_agent.exe",
    };
    for (const auto& item : {
             std::tuple{"bin/Smile2UnlockAuthService.exe", installed.auth_service, true},
             std::tuple{"bin/su_recognition_agent.exe", installed.recognition_agent, true},
             std::tuple{"bin/su_app.exe", install_bin / "su_app.exe", true},
             std::tuple{"bin/su_deploy_helper.exe", install_bin / "su_deploy_helper.exe", true},
             std::tuple{"bin/su_password_tool.exe", install_bin / "su_password_tool.exe", true},
             std::tuple{"bin/su_credential_provider.dll", install_bin / "su_credential_provider.dll", true},
             std::tuple{"bin/Smile2Unlock.ico", install_bin / "Smile2Unlock.ico", false},
         }) {
        if (const auto copied = copy_required_file(
                *package, std::get<0>(item), std::get<1>(item), std::get<2>(item)); !copied) {
            return std::unexpected(copied.error());
        }
    }

    // The agent is dynamically linked to SeetaFace and the MinGW runtime.
    // Copy only the known runtime set; unrelated DLLs beside the package are
    // never promoted into the trusted install directory.
    constexpr auto runtime_names = std::array{
        L"libgcc_s_seh-1.dll", L"libstdc++-6.dll", L"libwinpthread-1.dll",
        L"libgomp-1.dll", L"libSeetaFaceAntiSpoofingX600.dll",
        L"libSeetaFaceDetector600.dll", L"libSeetaFaceLandmarker600.dll",
        L"libSeetaFaceRecognizer610.dll", L"libSeetaAuthorize.dll", L"libtennis.dll",
    };
    for (const auto* name : runtime_names) {
        const auto relative = "bin/" + wide_to_utf8(name);
        if (const auto copied = copy_required_file(
                *package, relative, install_bin / name, true); !copied) {
            return std::unexpected(copied.error());
        }
    }

    constexpr auto model_names = std::array{
        L"face_detector.csta", L"face_landmarker_pts5.csta", L"face_recognizer.csta",
        L"fas_first.csta", L"fas_second.csta",
    };
    for (const auto* name : model_names) {
        const auto relative = "assets/models/seeta/" + wide_to_utf8(name);
        if (const auto copied = copy_required_file(
                *package, relative, install_models / name, false); !copied) {
            return std::unexpected(copied.error());
        }
    }
    for (const auto* name : {L"en.json", L"zh-CN.json"}) {
        const auto relative = "assets/i18n/" + wide_to_utf8(name);
        if (const auto copied = copy_required_file(
                *package, relative, install_i18n / name, false); !copied) {
            return std::unexpected(copied.error());
        }
    }
    return installed;
}

std::expected<std::filesystem::path, std::string> stage_credential_provider(
    const std::filesystem::path& source_bin) {
    const auto package = load_package_manifest(source_bin);
    if (!package) {
        return std::unexpected(package.error());
    }
    const auto program_files = program_files_root();
    if (!program_files) {
        return std::unexpected(program_files.error());
    }
    const auto install_root = *program_files / "Smile2Unlock";
    const auto install_bin = install_root / "bin";
    auto error = std::error_code{};
    std::filesystem::create_directories(install_bin, error);
    if (error) {
        return std::unexpected("failed to create the protected install directory: "
            + error.message());
    }
    // Keep a stable provider path in the registry. LogonUI loads the DLL for
    // the lifetime of a logon session, so an in-use update is rejected with a
    // clear error instead of silently switching the registry to an opaque
    // content-addressed filename. The user can sign out or reboot, then retry.
    const auto destination = install_bin / "su_credential_provider.dll";
    for (const auto& directory : {install_root, install_bin}) {
        if (const auto checked = ensure_plain_directory(directory); !checked) {
            return std::unexpected(checked.error());
        }
    }
    if (const auto copied = copy_required_file(
            *package, "bin/su_credential_provider.dll", destination, true); !copied) {
        return std::unexpected(copied.error()
            + "; sign out or reboot Windows before updating the credential provider");
    }
    return destination;
}

std::expected<void, std::string> verify_manifest_file(
    const PackageManifest& package, std::string_view relative, bool executable) {
    const auto expected = package.hashes.find(std::string(relative));
    if (expected == package.hashes.end()) {
        return std::unexpected("required deployment file is absent from signed release-info: "
            + std::string(relative));
    }
    const auto path = package.root / std::filesystem::path{relative};
    auto file = open_plain_file(path);
    if (!file) {
        return std::unexpected(file.error());
    }
    if (executable) {
        if (const auto verified = verify_authenticode(file->get(), path); !verified) {
            return std::unexpected(verified.error());
        }
    }
    const auto bytes = read_all(file->get(), 512 * 1024 * 1024);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!::CryptAcquireContextW(
            &provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)
        || !::CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)
        || !::CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes->data()),
            static_cast<DWORD>(bytes->size()), 0)) {
        if (hash != 0) {
            ::CryptDestroyHash(hash);
        }
        if (provider != 0) {
            ::CryptReleaseContext(provider, 0);
        }
        return std::unexpected(win32_error("failed to hash signed deployment input"));
    }
    auto digest = std::array<BYTE, 32>{};
    DWORD digest_size = static_cast<DWORD>(digest.size());
    const auto finished = ::CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digest_size, 0);
    ::CryptDestroyHash(hash);
    ::CryptReleaseContext(provider, 0);
    if (!finished) {
        return std::unexpected(win32_error("failed to finish deployment input hash"));
    }
    auto expected_digest = expected->second;
    std::ranges::transform(expected_digest, expected_digest.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (hex_digest(digest.data(), digest_size) != expected_digest) {
        return std::unexpected("signed release-info hash mismatch for " + std::string(relative));
    }
    return {};
}

std::filesystem::path current_binary_directory() {
    auto path = std::wstring(32768, L'\0');
    const auto length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    auto wide = std::wstring(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    auto narrow = std::string(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), length,
        nullptr, nullptr);
    return narrow;
}

std::optional<std::wstring> reg_read_string(HKEY root, const wchar_t* path, const wchar_t* name) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::wstring buffer(512, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    const auto status = ::RegQueryValueExW(key, name, nullptr, nullptr,
        reinterpret_cast<BYTE*>(buffer.data()), &size);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return std::nullopt;
    }
    buffer.resize(size / sizeof(wchar_t) - (buffer[size / sizeof(wchar_t) - 1] == L'\0' ? 1 : 0));
    return buffer;
}

bool file_exists(const std::wstring& path) {
    const auto attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string json_escape(std::string_view text) {
    auto escaped = std::string{};
    escaped.reserve(text.size());
    for (const auto ch : text) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                escaped += std::format("\\u{:04x}", static_cast<unsigned char>(ch));
            } else {
                escaped += ch;
            }
        }
    }
    return escaped;
}

}  // namespace

bool process_elevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const auto queried = ::GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &size);
    ::CloseHandle(token);
    return queried && elevation.TokenIsElevated != 0;
}

std::expected<void, std::string> validate_deployment_package() {
    const auto package = load_package_manifest(current_binary_directory());
    if (!package) {
        return std::unexpected(package.error());
    }
    // This check happens in the unelevated GUI before ShellExecuteEx(runas),
    // preventing a replaced helper from becoming the UAC elevation target.
    return verify_manifest_file(*package, "bin/su_deploy_helper.exe", true);
}

bool credential_provider_enrolled() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCredentialProviderKeyPath, 0,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD disabled = 0;
    DWORD type = 0;
    DWORD size = sizeof(disabled);
    const auto status = ::RegQueryValueExW(
        key, L"Disabled", nullptr, &type, reinterpret_cast<BYTE*>(&disabled), &size);
    ::RegCloseKey(key);
    return status == ERROR_FILE_NOT_FOUND
        || (status == ERROR_SUCCESS && type == REG_DWORD && disabled == 0);
}

bool credential_provider_registered() {
    const auto dll = credential_provider_dll_path();
    return !dll.empty() && file_exists(utf8_to_wide(dll));
}

std::string credential_provider_dll_path() {
    const auto inproc = std::wstring(kClsidKeyPath) + L"\\InprocServer32";
    const auto dll = reg_read_string(HKEY_LOCAL_MACHINE, inproc.c_str(), L"");
    return dll ? wide_to_utf8(*dll) : std::string{};
}

bool auth_service_installed() {
    const auto manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return false;
    }
    const auto service = ::OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    if (service != nullptr) {
        ::CloseServiceHandle(service);
    }
    ::CloseServiceHandle(manager);
    return service != nullptr;
}

bool auth_service_running() {
    const auto manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return false;
    }
    const auto service = ::OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        ::CloseServiceHandle(manager);
        return false;
    }
    SERVICE_STATUS status{};
    const auto queried = ::QueryServiceStatus(service, &status);
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    return queried && status.dwCurrentState == SERVICE_RUNNING;
}

std::string auth_service_binary_path() {
    const auto manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return {};
    }
    const auto service = ::OpenServiceW(manager, kServiceName, SERVICE_QUERY_CONFIG);
    if (service == nullptr) {
        ::CloseServiceHandle(manager);
        return {};
    }
    DWORD needed = 0;
    (void)::QueryServiceConfigW(service, nullptr, 0, &needed);
    auto buffer = std::vector<BYTE>(needed);
    LPQUERY_SERVICE_CONFIGW config = nullptr;
    if (needed > 0) {
        config = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
        (void)::QueryServiceConfigW(service, config, needed, &needed);
    }
    std::string path;
    if (config != nullptr && config->lpBinaryPathName != nullptr) {
        path = wide_to_utf8(config->lpBinaryPathName);
    }
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    return path;
}

std::expected<DeploymentSnapshot, std::string> inspect_deployment() {
    auto snapshot = DeploymentSnapshot{};
    const auto dll = credential_provider_dll_path();
    const auto enrolled = credential_provider_enrolled();
    snapshot.targets.push_back(ComponentStatus{
        .id = std::string(kCredentialProviderId),
        .service = "Smile2Unlock Credential Provider",
        .effective_path = dll,
        .role = "login-and-lock",
        .state = enrolled ? "managed" : (dll.empty() ? "absent" : "supported"),
        .detail = dll.empty()
            ? "credential provider DLL is not registered"
            : (enrolled ? "enrolled in the logon UI"
                        : "CLSID registered but not enrolled in the logon UI"),
        .configured = enrolled,
        .configurable = true,
        .managed = enrolled,
    });

    const auto service_binary = auth_service_binary_path();
    const auto service_running = auth_service_running();
    auto service_path_managed = false;
    if (const auto program_files = program_files_root(); program_files && !service_binary.empty()) {
        auto configured_path = std::filesystem::path{utf8_to_wide(service_binary)};
        auto configured_text = configured_path.wstring();
        if (configured_text.size() >= 2 && configured_text.front() == L'"'
            && configured_text.back() == L'"') {
            configured_path = configured_text.substr(1, configured_text.size() - 2);
        }
        const auto expected_path = *program_files / "Smile2Unlock" / "bin"
            / "Smile2UnlockAuthService.exe";
        auto error = std::error_code{};
        service_path_managed = std::filesystem::equivalent(
            configured_path, expected_path, error) && !error;
    }
    const auto service_ready = service_running && service_path_managed;
    snapshot.targets.push_back(ComponentStatus{
        .id = std::string(kAuthServiceId),
        .service = "Smile2Unlock Auth Service",
        .effective_path = service_binary,
        .role = "login-and-lock",
        .state = auth_service_installed()
            ? (!service_path_managed ? "conflict"
                : (service_ready ? "managed" : "supported"))
            : "absent",
        .detail = !auth_service_installed()
            ? "auth service is not installed"
            : (!service_path_managed ? "service uses a legacy or unmanaged binary path"
                : (service_running ? "service is running"
                                   : "service is installed but not running")),
        .configured = service_ready,
        .configurable = true,
        .managed = service_ready,
    });
    return snapshot;
}

std::expected<void, std::string> register_credential_provider() {
    const auto installed_dll = stage_credential_provider(current_binary_directory());
    if (!installed_dll) {
        return std::unexpected(installed_dll.error());
    }
    const auto wide_dll = installed_dll->wstring();
    if (wide_dll.empty() || !file_exists(wide_dll)) {
        return std::unexpected("credential provider DLL was not installed");
    }

    HKEY clsid_key = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kClsidKeyPath, 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &clsid_key, nullptr) != ERROR_SUCCESS) {
        return std::unexpected("failed to open the CLSID registry key");
    }
    const auto clsid_name_status = ::RegSetValueExW(clsid_key, L"", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(L"Smile2Unlock"), sizeof(L"Smile2Unlock"));
    ::RegCloseKey(clsid_key);
    if (clsid_name_status != ERROR_SUCCESS) {
        return std::unexpected("failed to write the CLSID display name");
    }

    const auto inproc_path = std::wstring(kClsidKeyPath) + L"\\InprocServer32";
    HKEY inproc_key = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, inproc_path.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &inproc_key, nullptr) != ERROR_SUCCESS) {
        return std::unexpected("failed to open the InprocServer32 registry key");
    }
    const auto dll_bytes = (wide_dll.size() + 1) * sizeof(wchar_t);
    const auto dll_status = ::RegSetValueExW(inproc_key, L"", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(wide_dll.c_str()), static_cast<DWORD>(dll_bytes));
    constexpr wchar_t kApartment[] = L"Apartment";
    const auto threading_status = ::RegSetValueExW(inproc_key, L"ThreadingModel", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(kApartment), sizeof(kApartment));
    ::RegCloseKey(inproc_key);
    if (dll_status != ERROR_SUCCESS || threading_status != ERROR_SUCCESS) {
        return std::unexpected("failed to write the InprocServer32 registration");
    }

    // LogonUI discovers credential providers by CLSID-named subkeys.
    HKEY cp_key = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kCredentialProviderKeyPath, 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &cp_key, nullptr) != ERROR_SUCCESS) {
        return std::unexpected("failed to open the CredentialProviders registry key");
    }
    constexpr wchar_t kProviderName[] = L"Smile2Unlock Credential Provider";
    const auto enrollment_status = ::RegSetValueExW(cp_key, L"", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(kProviderName), sizeof(kProviderName));
    const auto enabled_status = ::RegDeleteValueW(cp_key, L"Disabled");
    ::RegCloseKey(cp_key);
    if (enrollment_status != ERROR_SUCCESS
        || (enabled_status != ERROR_SUCCESS && enabled_status != ERROR_FILE_NOT_FOUND)) {
        return std::unexpected("failed to enroll the credential provider");
    }
    return {};
}

std::expected<void, std::string> unregister_credential_provider() {
    const auto removed = ::RegDeleteTreeW(HKEY_LOCAL_MACHINE, kCredentialProviderKeyPath);
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
        return std::unexpected("failed to remove the CredentialProviders registration");
    }
    // Remove values written by early development builds, which incorrectly
    // stored the CLSID directly below the parent key.
    HKEY cp_key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCredentialProvidersKeyPath, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &cp_key) == ERROR_SUCCESS) {
        for (DWORD index = 0;; ++index) {
            wchar_t name[64] = {};
            DWORD name_size = 64;
            BYTE value[256] = {};
            DWORD value_size = sizeof(value);
            const auto status = ::RegEnumValueW(
                cp_key, index, name, &name_size, nullptr, nullptr, value, &value_size);
            if (status == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (status != ERROR_SUCCESS) {
                continue;
            }
            std::wstring text(reinterpret_cast<wchar_t*>(value),
                value_size / sizeof(wchar_t));
            while (!text.empty() && text.back() == L'\0') {
                text.pop_back();
            }
            if (text == kClsid) {
                ::RegDeleteValueW(cp_key, name);
                break;
            }
        }
        ::RegCloseKey(cp_key);
    }
    // Keep the CLSID/InprocServer32 registration for diagnostics.
    return {};
}

std::expected<void, std::string> ensure_auth_service() {
    const auto manager = ::OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr) {
        return std::unexpected("failed to open the service control manager");
    }
    auto service = ::OpenServiceW(
        manager,
        kServiceName,
        SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
    if (service != nullptr) {
        SERVICE_STATUS current{};
        if (::QueryServiceStatus(service, &current)
            && current.dwCurrentState != SERVICE_STOPPED) {
            (void)::ControlService(service, SERVICE_CONTROL_STOP, &current);
            for (int attempt = 0; attempt < 50; ++attempt) {
                ::Sleep(100);
                if (::QueryServiceStatus(service, &current)
                    && current.dwCurrentState == SERVICE_STOPPED) {
                    break;
                }
            }
            if (current.dwCurrentState != SERVICE_STOPPED) {
                ::CloseServiceHandle(service);
                ::CloseServiceHandle(manager);
                return std::unexpected("timed out while stopping the auth service for update");
            }
        }
    }
    const auto installed = stage_security_components(current_binary_directory());
    if (!installed) {
        if (service != nullptr) {
            (void)::StartServiceW(service, 0, nullptr);
            ::CloseServiceHandle(service);
        }
        ::CloseServiceHandle(manager);
        return std::unexpected(installed.error());
    }
    if (service == nullptr) {
        if (::GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
            ::CloseServiceHandle(manager);
            return std::unexpected("failed to open the auth service");
        }
        const auto service_binary = installed->auth_service.wstring();
        if (service_binary.empty() || !file_exists(service_binary)) {
            ::CloseServiceHandle(manager);
            return std::unexpected("auth service binary is not deployed next to the helper");
        }
        const auto quoted_binary = L"\"" + service_binary + L"\"";
        service = ::CreateServiceW(
            manager,
            kServiceName,
            L"Smile2Unlock Authentication Service",
            SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            quoted_binary.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        if (service == nullptr) {
            ::CloseServiceHandle(manager);
            return std::unexpected("failed to register the auth service");
        }
    }
    const auto quoted_binary = L"\"" + installed->auth_service.wstring() + L"\"";
    if (!::ChangeServiceConfigW(
            service,
            SERVICE_NO_CHANGE,
            SERVICE_AUTO_START,
            SERVICE_NO_CHANGE,
            quoted_binary.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr)) {
        ::CloseServiceHandle(service);
        ::CloseServiceHandle(manager);
        return std::unexpected("failed to move the auth service to Program Files");
    }
    const auto started = ::StartServiceW(service, 0, nullptr);
    const auto start_error = started ? ERROR_SUCCESS : ::GetLastError();
    auto final_state = SERVICE_STOPPED;
    auto final_error = start_error;
    if (started || start_error == ERROR_SERVICE_ALREADY_RUNNING) {
        SERVICE_STATUS_PROCESS status{};
        DWORD status_size = 0;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (!::QueryServiceStatusEx(
                    service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status),
                    sizeof(status), &status_size)) {
                final_error = ::GetLastError();
                break;
            }
            final_state = status.dwCurrentState;
            if (final_state == SERVICE_RUNNING) {
                final_error = NO_ERROR;
                break;
            }
            if (final_state == SERVICE_STOPPED) {
                final_error = status.dwWin32ExitCode != NO_ERROR
                    ? status.dwWin32ExitCode
                    : ERROR_SERVICE_NOT_ACTIVE;
                break;
            }
            ::Sleep(100);
        }
        if (final_state != SERVICE_RUNNING && final_error == NO_ERROR) {
            final_error = ERROR_SERVICE_REQUEST_TIMEOUT;
        }
    }
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    if (final_state != SERVICE_RUNNING) {
        return std::unexpected(std::format(
            "failed to start the auth service (Win32 error {})", final_error));
    }
    return {};
}

std::string snapshot_json(const DeploymentSnapshot& snapshot) {
    std::string json = "[";
    for (std::size_t i = 0; i < snapshot.targets.size(); ++i) {
        const auto& target = snapshot.targets[i];
        if (i > 0) {
            json += ',';
        }
        json += std::format(
            R"({{"id":"{}","service":"{}","effective_path":"{}","role":"{}","state":"{}","detail":"{}","configured":{},"configurable":{},"managed":{},"wallet_available":false,"wallet_enabled":false}})",
            json_escape(target.id), json_escape(target.service), json_escape(target.effective_path),
            json_escape(target.role), json_escape(target.state), json_escape(target.detail),
            target.configured ? "true" : "false",
            target.configurable ? "true" : "false",
            target.managed ? "true" : "false");
    }
    json += "]";
    return json;
}

}  // namespace su::windeploy
