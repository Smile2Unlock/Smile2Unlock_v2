#pragma once

#include "face_profile_store.h"
#include "common/log/logger.h"
#include "logon_secret_protocol.h"
#include <span>
#include "logon_secret_store.h"
#include "request_worker_pool.h"

#include <windows.h>

#include <expected>
#include <memory>
#include <mutex>

namespace su::windows::auth_service {

class ManagementAuthorizer;
class SidRateLimiter;

class LogonSecretServer {
public:
    explicit LogonSecretServer(security::StorageKey storage_key, su::log::Logger logger);
    ~LogonSecretServer();
    LogonSecretServer(const LogonSecretServer&) = delete;
    LogonSecretServer& operator=(const LogonSecretServer&) = delete;

    std::expected<void, DWORD> serve(HANDLE stop_event);

private:
    bool handle_connection(HANDLE pipe, HANDLE stop_event);
    void execute_request(HANDLE pipe, HANDLE stop_event,
                         smile2unlock::logon_secret_ipc::Request& request,
                         std::span<const std::uint8_t> sid, DWORD pid);
    void reject_request(HANDLE pipe, HANDLE stop_event,
                        smile2unlock::logon_secret_ipc::Request& request,
                        smile2unlock::logon_secret_ipc::Status status);
    const su::log::Logger logger_;
    security::StorageKey storage_key_;
    security::LogonSecretStore secret_store_;
    security::FaceProfileStore profile_store_;
    std::unique_ptr<ManagementAuthorizer> management_authorizer_;
    std::unique_ptr<SidRateLimiter> rate_limiter_;
    // Serializes secret-store and profile-store file operations across pool
    // workers; the long recognition-agent run stays outside this lock.
    std::mutex state_mutex_;
    RequestWorkerPool worker_pool_;
};

} // namespace su::windows::auth_service
