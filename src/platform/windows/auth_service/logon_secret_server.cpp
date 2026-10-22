#include "logon_secret_server.h"
#include "request_log.h"

#include "client_disconnect_watcher.h"
#include "logon_secret_protocol.h"
#include "pipe_response.h"
#include "recognition_agent_protocol.h"
#include "recognition_result.h"
#include "sid_rate_limiter.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "server_helpers.h"

namespace su::windows::auth_service {
using namespace detail;

LogonSecretServer::LogonSecretServer(security::StorageKey storage_key, log::Logger logger)
    : logger_(std::move(logger)), storage_key_(std::move(storage_key)), secret_store_(storage_key_),
      profile_store_(storage_key_),
      management_authorizer_(std::make_unique<ManagementAuthorizer>()),
      rate_limiter_(std::make_unique<SidRateLimiter>()) {}

LogonSecretServer::~LogonSecretServer() = default;

std::expected<void, DWORD> LogonSecretServer::serve(HANDLE stop_event) {
    const auto descriptor = pipe_descriptor();
    if (!descriptor) {
        logger_.error("pipe.descriptor_failed", log::win32_error_fields(descriptor.error()));
        return std::unexpected(descriptor.error());
    }
    auto attributes = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), descriptor->get(), FALSE};
    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        auto pipe = ScopedHandle{CreateNamedPipeW(
            smile2unlock::logon_secret_ipc::kPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, sizeof(Response), sizeof(Request), 3000, &attributes)};
        if (pipe.get() == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            logger_.error("pipe.create_failed", log::win32_error_fields(error));
            return std::unexpected(error);
        }
        logger_.debug("pipe.listening");
        const auto connected = wait_for_pipe_client(pipe.get(), stop_event);
        if (!connected) {
            if (connected.error() == ERROR_OPERATION_ABORTED ||
                WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0)
                break;
            logger_.warn("pipe.connect_failed", log::win32_error_fields(connected.error()));
            continue;
        }
        if (!*connected)
            break;
        if (handle_connection(pipe.get(), stop_event))
            pipe.release();
    }
    return {};
}

bool LogonSecretServer::handle_connection(HANDLE pipe, HANDLE stop_event) {
    const auto sid = client_sid(pipe);
    const auto pid = client_process_id(pipe);
    const auto text =
        sid ? sid_string(*sid)
            : std::expected<std::wstring, DWORD>{std::unexpected(ERROR_ACCESS_DENIED)};
    if (!sid || !pid || !text || !rate_limiter_->allow_connection(*text)) {
        logger_.warn("pipe.identity_rejected",
                     {log::field("sid_error", sid ? 0ul : sid.error()),
                      log::field("pid_error", pid ? 0ul : pid.error()),
                      log::field("text_error", text ? 0ul : text.error())});
        (void)DisconnectNamedPipe(pipe);
        return false;
    }
    auto request = Request{};
    auto bytes_read = DWORD{0};
    const auto read = read_request(pipe, stop_event, request, bytes_read);
    if (!read || !*read) {
        logger_.warn("pipe.read_failed",
                     log::win32_error_fields(read ? ERROR_HANDLE_EOF : read.error()));
        smile2unlock::logon_secret_ipc::clear_request(request);
        return false;
    }
    const auto valid_size = bytes_read == sizeof(request);
    if (valid_size && rate_limiter_->allow_request(*text)) {
        const auto submitted = worker_pool_.submit(
            [this, pipe, stop_event, request, sid = *sid, pid = *pid]() mutable {
                execute_request(pipe, stop_event, request, sid, pid);
            });
        smile2unlock::logon_secret_ipc::clear_request(request);
        if (submitted)
            return true;
        logger_.warn("request.capacity_rejected");
        reject_request(pipe, stop_event, request, Status::kUnavailable);
        return false;
    }
    reject_request(pipe, stop_event, request, Status::kInvalidRequest);
    return false;
}

void LogonSecretServer::execute_request(HANDLE pipe, HANDLE stop_event, Request& request,
                                        std::span<const std::uint8_t> sid, DWORD pid) {
    const auto pipe_owner = ScopedHandle{pipe};
    auto response = Response{};
    response.request_id = request.request_id;
    response.logon_session_id = request.logon_session_id;
    const auto identity = sid_string(sid);
    const auto text =
        identity ? wide_to_utf8(*identity)
                 : std::expected<std::string, Status>{std::unexpected(Status::kUnavailable)};
    auto trace = RequestLog{logger_.with({log::field("request_id", request.request_id),
                                          log::field("op", operation_name(request.operation)),
                                          log::field("sid", text ? *text : "unavailable")})};
    auto watcher = ClientDisconnectWatcher{pipe};
    response.status =
        process_request(stop_event, watcher.event(), request, response, secret_store_,
                        profile_store_, *management_authorizer_, state_mutex_, sid, pid, trace);
    trace.completed(response.status);
    watcher.finish();
    smile2unlock::logon_secret_ipc::clear_request(request);
    const auto written =
        write_pipe_response(pipe, stop_event, std::as_bytes(std::span{&response, 1}));
    if (!written)
        trace.logger.warn("pipe.response_failed", log::win32_error_fields(written.error()));
    smile2unlock::logon_secret_ipc::clear_response(response);
    (void)DisconnectNamedPipe(pipe);
}

void LogonSecretServer::reject_request(HANDLE pipe, HANDLE stop_event, Request& request,
                                       Status status) {
    auto response = Response{};
    // Keep the existing zero response identity for invalid/limited requests.
    if (status == Status::kUnavailable) {
        response.request_id = request.request_id;
        response.logon_session_id = request.logon_session_id;
    }
    response.status = status;
    auto trace = RequestLog{logger_.with({log::field("request_id", request.request_id),
                                          log::field("op", operation_name(request.operation))})};
    trace.completed(response.status);
    smile2unlock::logon_secret_ipc::clear_request(request);
    const auto written =
        write_pipe_response(pipe, stop_event, std::as_bytes(std::span{&response, 1}));
    if (!written)
        trace.logger.warn("pipe.response_failed", log::win32_error_fields(written.error()));
    smile2unlock::logon_secret_ipc::clear_response(response);
    (void)DisconnectNamedPipe(pipe);
}

} // namespace su::windows::auth_service
