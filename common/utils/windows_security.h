#pragma once

#include <windows.h>
#include <aclapi.h>
#include <vector>

namespace smile2unlock::windows_security {
namespace detail {

inline bool CopySidToBuffer(PSID sid, std::vector<BYTE>& sid_buffer) {
    if (sid == nullptr || !IsValidSid(sid)) {
        return false;
    }

    const DWORD sid_size = GetLengthSid(sid);
    sid_buffer.resize(sid_size);
    return CopySid(sid_size, sid_buffer.data(), sid) == TRUE;
}

inline bool QueryActiveConsoleUserSid(std::vector<BYTE>& sid_buffer) {
    const DWORD session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xFFFFFFFF) {
        return false;
    }

    HMODULE wtsapi = LoadLibraryW(L"wtsapi32.dll");
    if (wtsapi == nullptr) {
        return false;
    }

    using WtsQueryUserTokenFn = BOOL(WINAPI*)(ULONG, PHANDLE);
    const auto query_user_token = reinterpret_cast<WtsQueryUserTokenFn>(
        GetProcAddress(wtsapi, "WTSQueryUserToken"));
    if (query_user_token == nullptr) {
        FreeLibrary(wtsapi);
        return false;
    }

    HANDLE token = nullptr;
    const BOOL query_ok = query_user_token(session_id, &token);
    FreeLibrary(wtsapi);
    if (!query_ok || token == nullptr) {
        return false;
    }

    DWORD required_size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required_size);
    if (required_size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(token);
        return false;
    }

    std::vector<BYTE> token_buffer(required_size);
    if (!GetTokenInformation(
            token,
            TokenUser,
            token_buffer.data(),
            required_size,
            &required_size)) {
        CloseHandle(token);
        return false;
    }

    CloseHandle(token);
    const auto* token_user =
        reinterpret_cast<const TOKEN_USER*>(token_buffer.data());
    return CopySidToBuffer(token_user->User.Sid, sid_buffer);
}

inline bool BuildFallbackInteractiveSid(std::vector<BYTE>& sid_buffer) {
    DWORD sid_size = SECURITY_MAX_SID_SIZE;
    sid_buffer.resize(sid_size);
    if (!CreateWellKnownSid(
            WinInteractiveSid,
            nullptr,
            sid_buffer.data(),
            &sid_size)) {
        sid_buffer.clear();
        return false;
    }

    sid_buffer.resize(sid_size);
    return true;
}

inline bool BuildLocalSystemSid(std::vector<BYTE>& sid_buffer) {
    DWORD sid_size = SECURITY_MAX_SID_SIZE;
    sid_buffer.resize(sid_size);
    if (!CreateWellKnownSid(
            WinLocalSystemSid,
            nullptr,
            sid_buffer.data(),
            &sid_size)) {
        sid_buffer.clear();
        return false;
    }

    sid_buffer.resize(sid_size);
    return true;
}

} // namespace detail

inline void FreeSecurityAcl(PACL& acl) {
    if (acl != nullptr) {
        LocalFree(acl);
        acl = nullptr;
    }
}

inline bool BuildServiceIpcSecurityAttributes(
    SECURITY_ATTRIBUTES& sa,
    SECURITY_DESCRIPTOR& sd,
    PACL& acl) {
    acl = nullptr;

    std::vector<BYTE> system_sid;
    if (!detail::BuildLocalSystemSid(system_sid)) {
        return false;
    }

    std::vector<BYTE> interactive_sid;
    if (!detail::QueryActiveConsoleUserSid(interactive_sid) &&
        !detail::BuildFallbackInteractiveSid(interactive_sid)) {
        return false;
    }

    EXPLICIT_ACCESSW access_entries[2]{};

    access_entries[0].grfAccessPermissions = GENERIC_ALL;
    access_entries[0].grfAccessMode = SET_ACCESS;
    access_entries[0].grfInheritance = NO_INHERITANCE;
    access_entries[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access_entries[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
    access_entries[0].Trustee.ptstrName =
        static_cast<LPWSTR>(reinterpret_cast<wchar_t*>(system_sid.data()));

    access_entries[1].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
    access_entries[1].grfAccessMode = SET_ACCESS;
    access_entries[1].grfInheritance = NO_INHERITANCE;
    access_entries[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access_entries[1].Trustee.TrusteeType = TRUSTEE_IS_USER;
    access_entries[1].Trustee.ptstrName =
        static_cast<LPWSTR>(reinterpret_cast<wchar_t*>(interactive_sid.data()));

    const DWORD acl_result = SetEntriesInAclW(
        2, access_entries, nullptr, &acl);
    if (acl_result != ERROR_SUCCESS) {
        SetLastError(acl_result);
        return false;
    }

    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
        FreeSecurityAcl(acl);
        return false;
    }

    if (!SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE)) {
        FreeSecurityAcl(acl);
        return false;
    }

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    return true;
}

} // namespace smile2unlock::windows_security
