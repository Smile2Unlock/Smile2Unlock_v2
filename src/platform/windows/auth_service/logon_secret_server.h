#pragma once

#include "face_profile_store.h"
#include "logon_secret_store.h"

#include <windows.h>

#include <expected>

namespace su::windows::auth_service {

class LogonSecretServer {
public:
    explicit LogonSecretServer(security::StorageKey storage_key);
    LogonSecretServer(const LogonSecretServer&) = delete;
    LogonSecretServer& operator=(const LogonSecretServer&) = delete;

    std::expected<void, DWORD> serve(HANDLE stop_event);

private:
    security::StorageKey storage_key_;
    security::LogonSecretStore secret_store_;
    security::FaceProfileStore profile_store_;
};

} // namespace su::windows::auth_service
