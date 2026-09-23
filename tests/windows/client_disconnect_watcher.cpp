#include "client_disconnect_watcher.h"

#include <chrono>
#include <iostream>
#include <thread>

namespace {

using su::windows::auth_service::ClientDisconnectWatcher;

struct PipePair {
    HANDLE server;
    HANDLE client;
};

// One overlapped server instance plus a connected client, message mode on
// both ends, mirroring how the auth service creates its pipes.
PipePair make_pair(const wchar_t* name) {
    const auto server = CreateNamedPipeW(
        name,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,
        4096,
        4096,
        0,
        nullptr);
    if (server == INVALID_HANDLE_VALUE) {
        return {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    }
    const auto connect_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (connect_event == nullptr) {
        CloseHandle(server);
        return {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    }
    auto overlapped = OVERLAPPED{};
    overlapped.hEvent = connect_event;
    auto pending = false;
    if (ConnectNamedPipe(server, &overlapped) == FALSE) {
        const auto error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            pending = true;
        } else if (error != ERROR_PIPE_CONNECTED) {
            CloseHandle(connect_event);
            CloseHandle(server);
            return {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
        }
    }
    // Connecting the client is what completes the pending ConnectNamedPipe,
    // so the client handle must be created before waiting on the event.
    const auto client = CreateFileW(
        name,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);
    if (client == INVALID_HANDLE_VALUE
        || (pending
            && WaitForSingleObject(connect_event, 2000) != WAIT_OBJECT_0)) {
        CloseHandle(connect_event);
        if (client != INVALID_HANDLE_VALUE) {
            CloseHandle(client);
        }
        CloseHandle(server);
        return {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    }
    CloseHandle(connect_event);
    return {server, client};
}

bool signaled(HANDLE event) {
    return WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

bool wait_signaled(HANDLE event, DWORD timeout_ms) {
    return WaitForSingleObject(event, timeout_ms) == WAIT_OBJECT_0;
}

} // namespace

int main() {
    // Client closing its handle signals the watcher while the transaction
    // would be running.
    {
        const auto pair = make_pair(L"\\\\.\\pipe\\Smile2UnlockTest.DisconnectLive");
        if (pair.server == INVALID_HANDLE_VALUE) {
            std::cerr << "pipe pair setup failed\n";
            return 1;
        }
        const char message[] = "request";
        auto written = DWORD{0};
        if (!WriteFile(
                pair.client, message, sizeof(message), &written, nullptr)) {
            std::cerr << "client write failed\n";
            return 1;
        }
        // Drain the message so the watcher's scratch read stays pending.
        char buffer[64]{};
        auto read = DWORD{0};
        if (!ReadFile(pair.server, buffer, sizeof(buffer), &read, nullptr)) {
            std::cerr << "server read failed\n";
            return 1;
        }
        auto watcher = ClientDisconnectWatcher{pair.server};
        if (watcher.event() == nullptr) {
            std::cerr << "watcher failed to start\n";
            return 1;
        }
        if (signaled(watcher.event())) {
            std::cerr << "watcher signaled while client still connected\n";
            return 1;
        }
        CloseHandle(pair.client);
        if (!wait_signaled(watcher.event(), 2000)) {
            std::cerr << "client close did not signal the watcher\n";
            return 1;
        }
        watcher.finish();
        // The pipe handle must still be usable and closable after the
        // OVERLAPPED was reclaimed.
        if (!DisconnectNamedPipe(pair.server)) {
            std::cerr << "disconnect failed after watcher reclaim\n";
            return 1;
        }
        CloseHandle(pair.server);
    }

    // Constructing the watcher after the client is already gone signals
    // immediately (the ReadFile completes at once with ERROR_BROKEN_PIPE).
    {
        const auto pair = make_pair(L"\\\\.\\pipe\\Smile2UnlockTest.DisconnectDead");
        if (pair.server == INVALID_HANDLE_VALUE) {
            std::cerr << "pipe pair setup failed (dead-client case)\n";
            return 1;
        }
        CloseHandle(pair.client);
        auto watcher = ClientDisconnectWatcher{pair.server};
        if (watcher.event() == nullptr) {
            std::cerr << "watcher failed to start (dead-client case)\n";
            return 1;
        }
        if (!signaled(watcher.event())) {
            std::cerr << "already-disconnected client did not signal\n";
            return 1;
        }
        watcher.finish();
        (void)DisconnectNamedPipe(pair.server);
        CloseHandle(pair.server);
    }

    // A client that stays connected for a while and then goes away is still
    // detected: nothing is signaled early, and the close lands.
    {
        const auto pair = make_pair(L"\\\\.\\pipe\\Smile2UnlockTest.DisconnectLater");
        if (pair.server == INVALID_HANDLE_VALUE) {
            std::cerr << "pipe pair setup failed (later case)\n";
            return 1;
        }
        auto watcher = ClientDisconnectWatcher{pair.server};
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
        if (signaled(watcher.event())) {
            std::cerr << "watcher signaled without a disconnect\n";
            return 1;
        }
        CloseHandle(pair.client);
        if (!wait_signaled(watcher.event(), 2000)) {
            std::cerr << "late client close did not signal the watcher\n";
            return 1;
        }
        watcher.finish();
        (void)DisconnectNamedPipe(pair.server);
        CloseHandle(pair.server);
    }

    std::cout << "client disconnect watcher: ok\n";
    return 0;
}
