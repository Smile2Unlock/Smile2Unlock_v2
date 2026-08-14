// diagnose_path.cpp — manually reproduce ensure_parent_directory step by
// step with GetLastError logging, so the failing call is unambiguous.
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <cstdio>
#include <string>

static void step(const char* name, bool ok, DWORD err) {
    std::printf("[diag] %-45s %s err=%lu\n", name, ok ? "OK" : "FAIL", err);
}

static int test_dir(const wchar_t* dir) {
    std::printf("[diag] ===== dir: %ls =====\n", dir);
    const wchar_t* sddl = L"D:P(A;;FA;;;SY)";

    // 1. SDDL -> descriptor
    PSECURITY_DESCRIPTOR sd = nullptr;
    BOOL ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl, SDDL_REVISION_1, &sd, nullptr);
    step("ConvertStringSecurityDescriptor", ok != FALSE, GetLastError());
    if (!ok) return 1;

    // 2. DACL out of descriptor
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    ok = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    step("GetSecurityDescriptorDacl", ok != FALSE, GetLastError());
    std::printf("[diag] dacl_present=%d dacl=%p\n", present, (void*)dacl);
    if (!ok || !present) return 1;

    // 3. Create directory if missing
    DWORD attr = GetFileAttributesW(dir);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;
        ok = CreateDirectoryW(dir, &sa);
        step("CreateDirectoryW", ok != FALSE, GetLastError());
    } else {
        std::printf("[diag] dir exists attrs=0x%lx\n", attr);
    }

    // 4. Open with WRITE_DAC
    HANDLE h = CreateFileW(
        dir,
        FILE_READ_ATTRIBUTES | WRITE_DAC,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    step("CreateFileW(WRITE_DAC)", h != INVALID_HANDLE_VALUE, GetLastError());
    if (h == INVALID_HANDLE_VALUE) return 1;

    // 5. File info
    BY_HANDLE_FILE_INFORMATION info{};
    ok = GetFileInformationByHandle(h, &info);
    step("GetFileInformationByHandle", ok != FALSE, GetLastError());
    if (ok) {
        std::printf("[diag] attrs=0x%lx reparse=%d\n", info.dwFileAttributes,
                    (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
    }

    // 6. SetSecurityInfo DACL (+protected)
    DWORD r = SetSecurityInfo(
        h, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr);
    step("SetSecurityInfo(DACL+PROTECTED)", r == ERROR_SUCCESS, r);
    if (r != ERROR_SUCCESS) {
        // 6b. DACL only
        r = SetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                            nullptr, nullptr, dacl, nullptr);
        step("SetSecurityInfo(DACL only)", r == ERROR_SUCCESS, r);
        // 6c. same DACL no-op
        r = SetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                            nullptr, nullptr, dacl, nullptr);
        step("SetSecurityInfo(DACL only 2)", r == ERROR_SUCCESS, r);
        // 6d. OWNER + DACL
        PSID system_sid = nullptr;
        ConvertStringSidToSidW(L"S-1-5-18", &system_sid);
        r = SetSecurityInfo(h, SE_FILE_OBJECT,
                            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                            system_sid, nullptr, dacl, nullptr);
        step("SetSecurityInfo(OWNER+SYSTEM+DACL)", r == ERROR_SUCCESS, r);
        LocalFree(system_sid);
    }

    CloseHandle(h);
    LocalFree(sd);
    std::printf("[diag] done for %ls\n", dir);
    return 0;
}

int main() {
    test_dir(L"C:\\ProgramData\\Smile2Unlock");
    test_dir(L"C:\\su-deploy\\diagtest");
    std::printf("[diag] all done\n");
    return 0;
}
