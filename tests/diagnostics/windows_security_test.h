#pragma once
#include "common/diagnostics/windows_security.h"
#include "common/log/windows_directory.h"
#include "common/utf8_path.h"
#include <aclapi.h>
#include <sddl.h>
#include <fstream>
#include <iostream>

namespace security_test {
using su::log::Handle;
namespace d = su::diagnostics;
namespace fs = std::filesystem;

inline Handle security_handle(const fs::path& path) {
    return Handle{CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
}

inline std::wstring security_text(const fs::path& path) {
    const auto handle = security_handle(path);
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (GetSecurityInfo(handle.get(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &raw) != ERROR_SUCCESS) std::exit(1);
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    LPWSTR text = nullptr;
    if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(raw, SDDL_REVISION_1,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &text, nullptr)) std::exit(1);
    const auto memory = std::unique_ptr<void, decltype(&LocalFree)>{text, &LocalFree};
    return text;
}

inline std::wstring user_sid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) std::exit(1);
    const auto token = Handle{raw};
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    auto bytes = std::vector<unsigned char>(size);
    if (!GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size)) std::exit(1);
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text))
        std::exit(1);
    const auto memory = std::unique_ptr<void, decltype(&LocalFree)>{text, &LocalFree};
    return text;
}

inline DWORD backup_attributes() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) std::exit(1);
    const auto token = Handle{raw};
    LUID backup{};
    if (!LookupPrivilegeValueW(nullptr, L"SeBackupPrivilege", &backup)) std::exit(1);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenPrivileges, nullptr, 0, &size);
    auto bytes = std::vector<unsigned char>(size);
    if (!GetTokenInformation(token.get(), TokenPrivileges, bytes.data(), size, &size)) std::exit(1);
    const auto* privileges = reinterpret_cast<TOKEN_PRIVILEGES*>(bytes.data());
    for (DWORD index = 0; index < privileges->PrivilegeCount; ++index) {
        const auto& value = privileges->Privileges[index];
        if (value.Luid.LowPart == backup.LowPart && value.Luid.HighPart == backup.HighPart)
            return value.Attributes;
    }
    return SE_PRIVILEGE_REMOVED;
}

inline void set_dacl(const fs::path& path, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &raw, nullptr))
        std::exit(1);
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(raw, &present, &dacl, &defaulted) || !present) std::exit(1);
    const auto handle = Handle{CreateFileW(path.c_str(), WRITE_DAC | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (SetSecurityInfo(handle.get(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr) != ERROR_SUCCESS) std::exit(1);
}

inline void write_administrator_file(const fs::path& path) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAD:P(A;;FA;;;BA)(A;;FA;;;SY)", SDDL_REVISION_1, &raw, nullptr)) std::exit(1);
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    auto attributes = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
    const auto file = Handle{CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    constexpr auto text = std::string_view{"protected system log"};
    DWORD written = 0;
    if (!WriteFile(file.get(), text.data(), text.size(), &written, nullptr) || written != text.size())
        std::exit(1);
}

template <typename Check>
void protected_root_checks(const fs::path& root, Check check) {
    const auto protected_root = root / "Smile2Unlock-Diagnostics";
    check(d::create_protected_directory(protected_root).has_value());
    const auto before = security_text(protected_root);
    check(before.starts_with(L"O:BA"));
    check(d::open_protected_directory(protected_root).has_value());
    check(d::create_protected_directory(protected_root).has_value());
    check(security_text(protected_root) == before);
    const auto child = protected_root / "0123456789abcdef";
    check(d::create_protected_directory(child).has_value());
    check(d::validate_staging_child(protected_root, su::path_utf8(child)).has_value());
    const auto link = root / "root-link";
    check(CreateSymbolicLinkW(link.c_str(), protected_root.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY));
    check(!d::create_protected_directory(link));
    check(!d::validate_staging_child(link, su::path_utf8(link / child.filename())));
    check(security_text(protected_root) == before);
}

template <typename Check>
void untrusted_root_checks(const fs::path& root, Check check) {
    const auto path = root / "untrusted-root";
    const auto sddl = L"O:" + user_sid() + L"D:P(A;OICI;FA;;;BA)";
    PSECURITY_DESCRIPTOR raw = nullptr;
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl.c_str(), SDDL_REVISION_1, &raw, nullptr));
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    auto attributes = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
    check(CreateDirectoryW(path.c_str(), &attributes));
    const auto before = security_text(path);
    check(!d::create_protected_directory(path));
    check(security_text(path) == before);
}

template <typename Check>
void backup_read_checks(const fs::path& root, Check check) {
    const auto wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    const auto product = root / (wine ? "BackupFixture" : "Smile2Unlock");
    const auto logs = product / "Logs";
    check(d::create_protected_directory(product).has_value());
    check(d::create_protected_directory(logs).has_value());
    write_administrator_file(logs / "service.log");
    // Keep fixture handles alive: Wine stores some security metadata only while open.
    const auto product_handle = security_handle(product);
    const auto logs_handle = security_handle(logs);
    const auto file_handle = security_handle(logs / "service.log");
    if (wine)
        std::cout << "SKIP: SYSTEM-only directory ACL enforcement under Wine; native Windows required\n";
    else
        set_dacl(product, L"D:P(A;OICI;FA;;;SY)");
    const auto product_security = security_text(product);
    const auto log_security = security_text(logs);
    const auto previous = backup_attributes();
    {
        const auto privilege = d::BackupPrivilege::acquire();
        check(privilege.has_value());
        check((backup_attributes() & SE_PRIVILEGE_ENABLED) != 0);
        const auto files = d::system_log_files(logs);
        check(files && files->size() == 1 && files->front().filename() == "service.log");
        const auto text = d::read_system_log_tail(logs / "service.log");
        check(text && *text == "protected system log");
        check(security_text(product) == product_security);
        check(security_text(logs) == log_security);
    }
    check(backup_attributes() == previous);
    // Restore only this test fixture's ACL so the temporary tree can be removed.
    set_dacl(product, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
}
} // namespace security_test
