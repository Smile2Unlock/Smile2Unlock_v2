import std;
import su.app.session;

int main() {
    if (std::getenv("XDG_SESSION_ID") == nullptr) {
        return 0;
    }

    const auto monitor = su::app::SessionLockMonitor([] {});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!monitor.available() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return monitor.available() ? 0 : 1;
}
