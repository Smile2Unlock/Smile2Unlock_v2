#include "common/log/windows_directory.h"
#include <aclapi.h>
#include <sddl.h>
#include <cassert>
#include <iostream>
#undef NDEBUG
#include <cassert>

using namespace su::log;
namespace {
std::wstring security_text(const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    assert(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
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
void untrusted_directory(const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BUD:P(A;OICI;FA;;;WD)",
                                                                SDDL_REVISION_1, &raw, nullptr));
    const auto descriptor = std::unique_ptr<void, decltype(&LocalFree)>{raw, &LocalFree};
    auto attributes = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
    assert(CreateDirectoryW(path.c_str(), &attributes));
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
} // namespace
int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      (L"su-log-security-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directory(root);
    untrusted_directory(root / L"untrusted");
    reparse(root);
    std::filesystem::remove_all(root);
}
