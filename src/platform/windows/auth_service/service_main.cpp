#include "common/log/logger.h"

#include "logon_secret_server.h"

#include "storage_key_provider.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace {

constexpr wchar_t kServiceName[] = L"Smile2UnlockAuthService";

SERVICE_STATUS_HANDLE service_status_handle = nullptr;
HANDLE stop_event = nullptr;
SERVICE_STATUS service_status{
    .dwServiceType = SERVICE_WIN32_OWN_PROCESS,
    .dwCurrentState = SERVICE_START_PENDING,
    .dwControlsAccepted = 0,
    .dwWin32ExitCode = NO_ERROR,
    .dwServiceSpecificExitCode = 0,
    .dwCheckPoint = 0,
    .dwWaitHint = 0,
};

void report_status(DWORD state, DWORD error = NO_ERROR, DWORD wait_hint = 0) {
    service_status.dwCurrentState = state;
    service_status.dwWin32ExitCode = error;
    service_status.dwWaitHint = wait_hint;
    service_status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        : 0;
    (void)SetServiceStatus(service_status_handle, &service_status);
}

DWORD WINAPI service_control_handler(
    DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        report_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        if (stop_event != nullptr) {
            SetEvent(stop_event);
        }
    }
    return NO_ERROR;
}

void WINAPI service_main(DWORD, PWSTR*) {
    const auto& logger = su::log::process_logger("auth-service", true, true);
    logger.info("service.starting");
    service_status_handle = RegisterServiceCtrlHandlerExW(
        kServiceName, service_control_handler, nullptr);
    if (service_status_handle == nullptr) {
        logger.error("service.register_failed", su::log::win32_error_fields(GetLastError()));
        return;
    }
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
        const auto error = GetLastError();
        logger.error("service.event_failed", su::log::win32_error_fields(error));
        report_status(SERVICE_STOPPED, error);
        return;
    }

    const auto key_path = su::windows::security::default_storage_key_path();
    auto storage_key = key_path.empty()
        ? std::expected<su::windows::security::StorageKey,
                        su::windows::security::StorageKeyError>{
              std::unexpected(su::windows::security::StorageKeyError::kUnavailable)}
        : su::windows::security::load_or_create_storage_key(key_path);
    if (!storage_key) {
        logger.error("storage.load_failed", {su::log::field("cause",
            su::windows::security::storage_key_error_message(storage_key.error()))});
        CloseHandle(stop_event);
        stop_event = nullptr;
        report_status(SERVICE_STOPPED, ERROR_SERVICE_NOT_ACTIVE);
        return;
    }
    logger.info("storage.ready");

    report_status(SERVICE_RUNNING);
    logger.info("service.running");
    auto server = su::windows::auth_service::LogonSecretServer{std::move(*storage_key), logger};
    const auto served = server.serve(stop_event);
    const auto exit_code = served ? NO_ERROR : served.error();
    logger.info("service.stopped", su::log::win32_error_fields(exit_code));
    CloseHandle(stop_event);
    stop_event = nullptr;
    report_status(SERVICE_STOPPED, exit_code);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SERVICE_TABLE_ENTRYW dispatch_table[]{
        {const_cast<PWSTR>(kServiceName), service_main},
        {nullptr, nullptr},
    };
    return StartServiceCtrlDispatcherW(dispatch_table) ? 0 : 1;
}
