#pragma once

#include "client_disconnect_watcher.h"

#include <expected>
#include <limits>
#include <span>

namespace su::windows::auth_service {

// Clients close their one-transaction pipe after reading the response. Use
// that existing acknowledgement instead of FlushFileBuffers, which can wait
// forever for an uncooperative reader. The write and acknowledgement share
// one deadline and both observe service shutdown.
[[nodiscard]] inline std::expected<void, DWORD>
write_pipe_response(HANDLE pipe, HANDLE stop_event, std::span<const std::byte> response,
                    DWORD timeout_ms = 3000) {
    if (response.empty() || response.size() > std::numeric_limits<DWORD>::max()) {
        return std::unexpected(ERROR_INVALID_PARAMETER);
    }
    const auto deadline = GetTickCount64() + timeout_ms;
    const auto wait = [stop_event, deadline](HANDLE event) {
        const auto now = GetTickCount64();
        const auto remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
        const HANDLE events[]{stop_event, event};
        return WaitForMultipleObjects(2, events, FALSE, remaining);
    };
    const auto write_event = detail::WatcherEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!write_event) {
        return std::unexpected(GetLastError());
    }
    auto client = ClientDisconnectWatcher{pipe};
    if (client.event() == nullptr) {
        return std::unexpected(ERROR_NOT_ENOUGH_MEMORY);
    }
    if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
        return std::unexpected(ERROR_OPERATION_ABORTED);
    }
    auto overlapped = OVERLAPPED{};
    overlapped.hEvent = write_event.get();
    auto written = DWORD{0};
    if (!WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written,
                   &overlapped)) {
        const auto error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            return std::unexpected(error);
        }
        const auto result = wait(write_event.get());
        if (result != WAIT_OBJECT_0 + 1) {
            const auto wait_error = result == WAIT_OBJECT_0  ? ERROR_OPERATION_ABORTED
                                    : result == WAIT_TIMEOUT ? ERROR_TIMEOUT
                                                             : GetLastError();
            (void)CancelIoEx(pipe, &overlapped);
            // Reclaim kernel access to the stack buffer and OVERLAPPED before
            // returning, including when cancellation races completion.
            (void)GetOverlappedResult(pipe, &overlapped, &written, TRUE);
            return std::unexpected(wait_error);
        }
        if (!GetOverlappedResult(pipe, &overlapped, &written, FALSE)) {
            return std::unexpected(GetLastError());
        }
    }
    if (written != response.size()) {
        return std::unexpected(ERROR_WRITE_FAULT);
    }
    const auto result = wait(client.event());
    if (result == WAIT_OBJECT_0 + 1) {
        return {};
    }
    return std::unexpected(result == WAIT_OBJECT_0  ? ERROR_OPERATION_ABORTED
                           : result == WAIT_TIMEOUT ? ERROR_TIMEOUT
                                                    : GetLastError());
}

} // namespace su::windows::auth_service
