#include "common/log/windows_directory.h"
#include <aclapi.h>
#include <sddl.h>
#include <cassert>
#include <iostream>
#undef NDEBUG
#include <cassert>

using namespace su::log;
namespace {
std::wstring security_text(HANDLE handle) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    assert(GetSecurityInfo(handle, SE_FILE_OBJECT,
                                 OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, nullptr,
                                 nullptr, nullptr, nullptr, &raw) == ERROR_SUCCESS);
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    LPWSTR text = nullptr;
    assert(ConvertSecurityDescriptorToStringSecurityDescriptorW(
        raw, SDDL_REVISION_1, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &text,
        nullptr));
    const auto memory = std::unique_ptr<void, decltype(&LocalFree)>{text, &LocalFree};
    return text;
}
Handle security_handle(const std::filesystem::path& path) {
    auto handle = Handle{CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                     nullptr)};
    assert(handle.get() != INVALID_HANDLE_VALUE);
    return handle;
}
std::wstring security_text(const std::filesystem::path& path) {
    const auto handle = security_handle(path);
    return security_text(handle.get());
}
void restore_test_access(HANDLE handle) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;BA)",
                                                                SDDL_REVISION_1, &raw, nullptr));
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    assert(GetSecurityDescriptorDacl(raw, &present, &dacl, &defaulted) && present);
    assert(SetSecurityInfo(handle, SE_FILE_OBJECT,
                            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                            nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS);
}
void owned_directory(const std::filesystem::path& path, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &raw,
                                                                nullptr));
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    auto attributes = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
    assert(CreateDirectoryW(path.c_str(), &attributes));
}
void untrusted_directory(const std::filesystem::path& path) {
    owned_directory(path, L"O:BUD:P(A;OICI;FA;;;WD)");
    const auto before = security_text(path);
    const auto result = create_system_directory(path, true);
    assert(!result && result.error() == SinkError::untrusted_owner);
    assert(security_text(path) == before);
}
void reparse(const std::filesystem::path& root) {
    const auto target = root / L"target";
    std::filesystem::create_directory(target);
    const auto before = security_text(target);
    const auto link = root / L"junction-like-link";
    assert(CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 2));
    const auto rejected = create_system_directory(link, true);
    assert(!rejected && rejected.error() == SinkError::reparse_point);
    assert(!check_directory_chain(link, false));
    assert(security_text(target) == before);
    const auto file = target / L"target.log";
    {
        auto sink = rotating_file_sink(file);
        assert(sink(Level::info, "original\n"));
    }
    const auto file_link = root / L"link.log";
    assert(CreateSymbolicLinkW(file_link.c_str(), file.c_str(), 2));
    assert(!rotating_file_sink(file_link)(Level::info, "attacker\n"));
    assert(std::filesystem::file_size(file) == 9);
}
void trusted_directories(const std::filesystem::path& root) {
    const auto product = root / L"Smile2Unlock";
    // Wine's test account is a regular user SID even in its administrator group.
    owned_directory(product, L"O:BAD:P(A;OICI;FA;;;BA)");
    owned_directory(product / L"Logs", L"O:BAD:P(A;OICI;FA;;;BA)");
    const auto product_handle = security_handle(product);
    const auto logs_handle = security_handle(product / L"Logs");
    assert(create_system_directory(product / L"Logs", true));
    assert(create_system_directory(product));
    const auto product_security = security_text(product_handle.get());
    const auto logs_security = security_text(logs_handle.get());
    assert(product_security.substr(product_security.find(L'(')) == L"(A;OICI;FA;;;SY)");
    assert(logs_security.substr(logs_security.find(L'(')) ==
           L"(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
        // Wine 11.20 does not report the protection bit after SetSecurityInfo.
        std::cout << "SKIP: DACL protection-bit assertion under Wine; native Windows required\n";
    } else {
        assert(product_security.contains(L"D:P"));
        assert(logs_security.contains(L"D:P"));
    }
    // The test account is not SYSTEM: retain its original handle to restore cleanup access.
    restore_test_access(product_handle.get());
}
} // namespace
int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      (L"su-log-security-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(root);
    untrusted_directory(root / L"untrusted");
    reparse(root);
    trusted_directories(root);
    std::filesystem::remove_all(root);
}
