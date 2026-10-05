#include "pipe_response.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using su::windows::auth_service::write_pipe_response;
using su::windows::auth_service::detail::WatcherEvent;

struct PipePair {
    WatcherEvent server;
    WatcherEvent client;
};

PipePair make_pair(int index) {
    const auto name = L"\\\\.\\pipe\\Smile2UnlockTest.Response." +
                      std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(index);
    auto pair = PipePair{
        WatcherEvent{CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
                                      4096, 4096, 0, nullptr)},
        {},
    };
    if (pair.server.get() == INVALID_HANDLE_VALUE) {
        pair.server.reset();
        return pair;
    }
    const auto event = WatcherEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event) {
        return {};
    }
    auto overlapped = OVERLAPPED{};
    overlapped.hEvent = event.get();
    const auto connected = ConnectNamedPipe(pair.server.get(), &overlapped);
    const auto pending = !connected && GetLastError() == ERROR_IO_PENDING;
    if (!connected && !pending && GetLastError() != ERROR_PIPE_CONNECTED) {
        return {};
    }
    pair.client.reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr));
    if (pair.client.get() == INVALID_HANDLE_VALUE ||
        (pending && WaitForSingleObject(event.get(), 2000) != WAIT_OBJECT_0)) {
        (void)CancelIoEx(pair.server.get(), &overlapped);
        auto transferred = DWORD{};
        if (pending) {
            (void)GetOverlappedResult(pair.server.get(), &overlapped, &transferred, TRUE);
        }
        return {};
    }
    auto mode = DWORD{PIPE_READMODE_MESSAGE};
    if (!SetNamedPipeHandleState(pair.client.get(), &mode, nullptr, nullptr)) {
        return {};
    }
    return pair;
}

bool response_case(int index, std::size_t size, bool read_response, bool stop_service) {
    auto pair = make_pair(index);
    const auto stop = WatcherEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!pair.server || !pair.client || !stop) {
        return false;
    }
    const auto response = std::vector<std::byte>(size, std::byte{42});
    auto received = std::atomic<bool>{false};
    auto client = std::jthread{};
    if (read_response) {
        client = std::jthread([handle = pair.client.release(), &received, &response] {
            const auto close = WatcherEvent{handle};
            // The server must retain its response until this delayed reader
            // receives it; immediate DisconnectNamedPipe would discard it.
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            auto buffer = std::vector<std::byte>(response.size());
            auto read = DWORD{};
            received.store(ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                                    nullptr) &&
                           read == response.size() && buffer == response);
        });
    }
    auto stopper = std::jthread{};
    if (stop_service) {
        stopper = std::jthread([handle = stop.get()] {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            (void)SetEvent(handle);
        });
    }
    const auto start = std::chrono::steady_clock::now();
    const auto result =
        write_pipe_response(pair.server.get(), stop.get(), response, stop_service ? 3000 : 300);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    (void)DisconnectNamedPipe(pair.server.get());
    if (client.joinable()) {
        client.join();
    }
    if (stopper.joinable()) {
        stopper.join();
    }
    const auto expected =
        read_response
            ? result.has_value() && received.load()
            : !result && result.error() == (stop_service ? ERROR_OPERATION_ABORTED : ERROR_TIMEOUT);
    return expected && elapsed < std::chrono::seconds{2};
}

} // namespace

int main() {
    // Cover buffered writes, writes blocked by backpressure, shutdown during
    // either phase, and a healthy request following the timed-out clients.
    if (!response_case(1, 64, false, false) || !response_case(2, 256 * 1024, false, false) ||
        !response_case(3, 64, false, true) || !response_case(4, 256 * 1024, false, true) ||
        !response_case(5, 64, true, false) || !response_case(6, 256 * 1024, true, false)) {
        std::cerr << "pipe response deadline, shutdown or delivery check failed\n";
        return 1;
    }
    std::cout << "pipe response deadline, shutdown and delivery: ok\n";
}
