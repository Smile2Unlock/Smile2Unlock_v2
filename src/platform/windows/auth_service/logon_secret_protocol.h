#pragma once

#include <windows.h>
#include <cstdint>

namespace smile2unlock::logon_secret_ipc {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\Smile2Unlock.LogonSecret.v1";
inline constexpr std::uint32_t kMagic = 0x53325350; // S2SP
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kSidCapacity = 185;
inline constexpr std::size_t kUsernameCapacity = 513;
inline constexpr std::size_t kPasswordCapacity = 513;

enum class Operation : std::uint16_t {
    kPrepare = 1,
    kMarkStale = 2,
    kStore = 3,
    kClear = 4,
};

enum class Status : std::uint32_t {
    kOk = 0,
    kInvalidRequest = 1,
    kAccessDenied = 2,
    kUnavailable = 3,
    kStaleOrConsumed = 4,
    kCorrupt = 5,
};

struct Request {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    Operation operation = Operation::kPrepare;
    std::uint64_t request_id = 0;
    std::uint32_t logon_session_id = 0;
    std::uint32_t account_kind = 0;
    wchar_t sid[kSidCapacity]{};
    wchar_t canonical_username[kUsernameCapacity]{};
    wchar_t password[kPasswordCapacity]{};
};

struct Response {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    std::uint16_t reserved = 0;
    std::uint64_t request_id = 0;
    std::uint32_t logon_session_id = 0;
    Status status = Status::kUnavailable;
    std::uint32_t password_length = 0;
    wchar_t password[kPasswordCapacity]{};
};

inline void clear_request(Request& request) noexcept {
    SecureZeroMemory(request.password, sizeof(request.password));
}

inline void clear_response(Response& response) noexcept {
    SecureZeroMemory(response.password, sizeof(response.password));
    response.password_length = 0;
}

} // namespace smile2unlock::logon_secret_ipc
