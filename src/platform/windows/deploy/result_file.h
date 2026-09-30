#ifndef SU_PLATFORM_WINDOWS_DEPLOY_RESULT_FILE_H_
#define SU_PLATFORM_WINDOWS_DEPLOY_RESULT_FILE_H_

#include <windows.h>

#include <expected>
#include <string>

namespace su::windeploy {

// Keep the helper and GUI on the same UTF-16 path. A failed lookup must not
// silently redirect the result to the process's working directory.
inline std::expected<std::wstring, DWORD> result_file_path() {
    auto directory = std::wstring(32768, L'\0');
    const auto length = ::GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
    if (length == 0) {
        return std::unexpected(::GetLastError());
    }
    if (length >= directory.size()) {
        return std::unexpected(DWORD{ERROR_INSUFFICIENT_BUFFER});
    }
    directory.resize(length);
    directory += L"su_deploy_result.json";
    return directory;
}

}  // namespace su::windeploy

#endif  // SU_PLATFORM_WINDOWS_DEPLOY_RESULT_FILE_H_
