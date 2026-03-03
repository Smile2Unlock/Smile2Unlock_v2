#include "Smile2Unlock/backend/managers/dll_injector.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <windows.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")

namespace fs = std::filesystem;

namespace smile2unlock {
namespace managers {

std::string DllInjector::GetSourceDllPath() const {
    char buffer[MAX_PATH];
    GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    fs::path exePath(buffer);
    fs::path dllPath = exePath.parent_path() / DLL_NAME;
    return dllPath.string();
}

std::string DllInjector::GetTargetDllPath() const {
    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    fs::path targetPath = fs::path(sysPath) / DLL_NAME;
    return targetPath.string();
}

bool DllInjector::IsInjected() const {
    std::string targetPath = GetTargetDllPath();
    return fs::exists(targetPath);
}

std::string DllInjector::CalculateFileHash(const std::string& filePath) const {
    if (!fs::exists(filePath)) {
        return "FILE_NOT_FOUND";
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        return "CANNOT_OPEN";
    }

    std::vector<unsigned char> buffer(std::istreambuf_iterator<char>(file), {});
    
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    
    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "CRYPTO_ERROR";
    }

    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "CRYPTO_ERROR";
    }

    if (!CryptHashData(hHash, buffer.data(), static_cast<DWORD>(buffer.size()), 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "CRYPTO_ERROR";
    }

    BYTE hash[32];
    DWORD hashLen = 32;
    
    if (!CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "CRYPTO_ERROR";
    }

    std::stringstream ss;
    for (DWORD i = 0; i < hashLen; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    return ss.str();
}

bool DllInjector::VerifyDllHash() const {
    std::string sourceHash = CalculateFileHash(GetSourceDllPath());
    std::string targetHash = CalculateFileHash(GetTargetDllPath());
    
    if (sourceHash == "FILE_NOT_FOUND" || targetHash == "FILE_NOT_FOUND") {
        return false;
    }

    return sourceHash == targetHash;
}

bool DllInjector::IsRegistryConfigured() const {
    HKEY hKey;
    LONG result = RegOpenKeyExA(HKEY_LOCAL_MACHINE, REGISTRY_KEY_CP, 0, KEY_READ, &hKey);
    if (result == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool DllInjector::InjectDll(std::string& error_message) {
    try {
        std::string sourcePath = GetSourceDllPath();
        if (!fs::exists(sourcePath)) {
            error_message = "Source DLL not found: " + sourcePath;
            return false;
        }

        std::string targetPath = GetTargetDllPath();
        fs::copy_file(sourcePath, targetPath, fs::copy_options::overwrite_existing);

        if (!ImportRegistry()) {
            error_message = "Failed to configure registry";
            return false;
        }

        error_message = "Injection successful";
        return true;
    } catch (const std::exception& e) {
        error_message = std::string("Exception: ") + e.what();
        return false;
    }
}

bool DllInjector::ImportRegistry() {
    HKEY hKey;
    DWORD disposition;
    
    LONG result = RegCreateKeyExA(
        HKEY_LOCAL_MACHINE,
        REGISTRY_KEY_CP,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_WRITE,
        nullptr,
        &hKey,
        &disposition
    );

    if (result == ERROR_SUCCESS) {
        const char* value = "SampleV2CredentialProvider";
        RegSetValueExA(hKey, nullptr, 0, REG_SZ, 
                      reinterpret_cast<const BYTE*>(value), 
                      static_cast<DWORD>(strlen(value) + 1));
        RegCloseKey(hKey);
    } else {
        return false;
    }

    result = RegCreateKeyExA(
        HKEY_CLASSES_ROOT,
        REGISTRY_KEY_CLSID,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_WRITE,
        nullptr,
        &hKey,
        &disposition
    );

    if (result == ERROR_SUCCESS) {
        const char* value = "SampleV2CredentialProvider";
        RegSetValueExA(hKey, nullptr, 0, REG_SZ,
                      reinterpret_cast<const BYTE*>(value),
                      static_cast<DWORD>(strlen(value) + 1));
        RegCloseKey(hKey);
    } else {
        return false;
    }

    std::string inprocKey = std::string(REGISTRY_KEY_CLSID) + "\\InprocServer32";
    result = RegCreateKeyExA(
        HKEY_CLASSES_ROOT,
        inprocKey.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_WRITE,
        nullptr,
        &hKey,
        &disposition
    );

    if (result == ERROR_SUCCESS) {
        const char* dllName = "SampleV2CredentialProvider.dll";
        RegSetValueExA(hKey, nullptr, 0, REG_SZ,
                      reinterpret_cast<const BYTE*>(dllName),
                      static_cast<DWORD>(strlen(dllName) + 1));

        const char* threadingModel = "Apartment";
        RegSetValueExA(hKey, "ThreadingModel", 0, REG_SZ,
                      reinterpret_cast<const BYTE*>(threadingModel),
                      static_cast<DWORD>(strlen(threadingModel) + 1));
        RegCloseKey(hKey);
    } else {
        return false;
    }

    return true;
}

std::string DllInjector::GetDllVersion(const std::string& dllPath) const {
    if (!fs::exists(dllPath)) {
        return "N/A";
    }

    DWORD dummy;
    DWORD size = GetFileVersionInfoSizeA(dllPath.c_str(), &dummy);
    if (size == 0) {
        return "Unknown";
    }

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoA(dllPath.c_str(), 0, size, data.data())) {
        return "Unknown";
    }

    VS_FIXEDFILEINFO* fileInfo = nullptr;
    UINT len = 0;
    if (VerQueryValueA(data.data(), "\\", (LPVOID*)&fileInfo, &len)) {
        std::stringstream ss;
        ss << HIWORD(fileInfo->dwFileVersionMS) << "."
           << LOWORD(fileInfo->dwFileVersionMS) << "."
           << HIWORD(fileInfo->dwFileVersionLS) << "."
           << LOWORD(fileInfo->dwFileVersionLS);
        return ss.str();
    }

    return "Unknown";
}

DllStatus DllInjector::GetStatus() {
    DllStatus status;
    status.is_injected = IsInjected();
    status.is_registry_configured = IsRegistryConfigured();
    status.source_path = GetSourceDllPath();
    status.target_path = GetTargetDllPath();
    status.source_hash = CalculateFileHash(status.source_path);
    status.target_hash = CalculateFileHash(status.target_path);
    status.hash_match = VerifyDllHash();
    status.source_version = GetDllVersion(status.source_path);
    status.target_version = GetDllVersion(status.target_path);
    status.version_match = (!status.source_version.empty() && 
                           !status.target_version.empty() &&
                           status.source_version == status.target_version);
    return status;
}

} // namespace managers
} // namespace smile2unlock
