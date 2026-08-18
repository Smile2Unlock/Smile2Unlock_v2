#pragma once

#include <windows.h>
#include <cstdint>

namespace smile2unlock::logon_secret_ipc {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\Smile2Unlock.LogonSecret.v1";
inline constexpr std::uint32_t kMagic = 0x53325350; // S2SP
inline constexpr std::uint16_t kVersion = 2;
inline constexpr std::size_t kSidCapacity = 185;
inline constexpr std::size_t kUsernameCapacity = 513;
inline constexpr std::size_t kPasswordCapacity = 513;
inline constexpr std::size_t kPayloadCapacity = 48 * 1024;

enum class Operation : std::uint16_t {
    kPrepare = 1,
    kMarkStale = 2,
    kStore = 3,
    kClear = 4,
    kAuthenticateAndPrepare = 5,
    kEnrollProfile = 6,
    kListProfiles = 7,
    kDeleteProfile = 8,
    kVerifyProfile = 9,
    kCredentialStatus = 10,
};

enum class Status : std::uint32_t {
    kOk = 0,
    kInvalidRequest = 1,
    kAccessDenied = 2,
    kUnavailable = 3,
    kStaleOrConsumed = 4,
    kCorrupt = 5,
    kAuthenticationFailed = 6,
    kProfileNotFound = 7,
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
    std::uint32_t payload_length = 0;
    std::uint8_t payload[kPayloadCapacity]{};
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
    std::uint32_t payload_length = 0;
    std::uint8_t payload[kPayloadCapacity]{};
};

// Keep the C++ wire layout locked to the #[repr(C)] Rust client. A change to
// either fixed-size protocol must update both sides deliberately.
static_assert(sizeof(Request) == 51608);
static_assert(sizeof(Response) == 50216);

inline void clear_request(Request& request) noexcept {
    SecureZeroMemory(request.password, sizeof(request.password));
    SecureZeroMemory(request.payload, sizeof(request.payload));
    request.payload_length = 0;
}

inline void clear_response(Response& response) noexcept {
    SecureZeroMemory(response.password, sizeof(response.password));
    SecureZeroMemory(response.payload, sizeof(response.payload));
    response.password_length = 0;
    response.payload_length = 0;
}

} // namespace smile2unlock::logon_secret_ipc
