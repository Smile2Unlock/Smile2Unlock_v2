#pragma once

#include "Smile2Unlock/backend/models/types.h"
#include <string>
#include <windows.h>

namespace smile2unlock {
namespace managers {

class DllInjector {
public:
    DllInjector() = default;
    ~DllInjector() = default;

    bool IsInjected() const;
    bool IsRegistryConfigured() const;
    std::string GetSourceDllPath() const;
    std::string GetTargetDllPath() const;
    std::string CalculateFileHash(const std::string& filePath) const;
    bool VerifyDllHash() const;
    bool InjectDll(std::string& error_message);
    std::string GetDllVersion(const std::string& dllPath) const;
    DllStatus GetStatus();

private:
    bool ImportRegistry();
    
    static constexpr const char* DLL_NAME = "SampleV2CredentialProvider.dll";
    static constexpr const char* REGISTRY_KEY_CP = 
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers\\{5fd3d285-0dd9-4362-8855-e0abaacd4af6}";
    static constexpr const char* REGISTRY_KEY_CLSID = 
        "CLSID\\{5fd3d285-0dd9-4362-8855-e0abaacd4af6}";
};

} // namespace managers
} // namespace smile2unlock
