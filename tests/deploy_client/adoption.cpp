#include "platform/linux/deploy_client/deployment_client.h"

#include <systemd/sd-bus.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

namespace {
struct PrivateBus {
    pid_t pid = -1;
    ~PrivateBus() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            ::waitpid(pid, nullptr, 0);
        }
    }
    bool start() {
        int pipe_fds[2];
        if (::pipe(pipe_fds) != 0)
            return false;
        pid = ::fork();
        if (pid == 0) {
            ::close(pipe_fds[0]);
            ::dup2(pipe_fds[1], STDOUT_FILENO);
            ::close(pipe_fds[1]);
            ::execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork", "--print-address=1",
                     static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::close(pipe_fds[1]);
        auto* input = ::fdopen(pipe_fds[0], "r");
        char address[4096]{};
        const auto ok = pid > 0 && input && ::fgets(address, sizeof(address), input);
        if (input)
            ::fclose(input);
        else
            ::close(pipe_fds[0]);
        if (!ok)
            return false;
        auto text = std::string(address);
        if (text.ends_with('\n'))
            text.pop_back();
        return ::setenv("DBUS_SYSTEM_BUS_ADDRESS", text.c_str(), 1) == 0;
    }
};
struct Service {
    std::atomic<int> plans{0};
    std::atomic<int> applies{0};
    std::string owner;
};
int plan(sd_bus_message* message, void* userdata, sd_bus_error*) {
    auto& state = *static_cast<Service*>(userdata);
    const char* target = nullptr;
    const char* source = nullptr;
    if (::sd_bus_message_read(message, "ss", &target, &source) < 0 ||
        std::string_view(target) != "greetd" || std::string_view(source) != "none")
        return ::sd_bus_reply_method_errorf(message, SD_BUS_ERROR_INVALID_ARGS, "invalid plan");
    state.owner = ::sd_bus_message_get_sender(message);
    ++state.plans;
    return ::sd_bus_reply_method_return(message, "ss", "reviewed-plan",
                                        "{\"operation\":\"adopt\"}");
}
int apply(sd_bus_message* message, void* userdata, sd_bus_error*) {
    auto& state = *static_cast<Service*>(userdata);
    const char* id = nullptr;
    if (::sd_bus_message_read(message, "s", &id) < 0 || std::string_view(id) != "reviewed-plan" ||
        state.owner != ::sd_bus_message_get_sender(message) || state.applies != 0)
        return ::sd_bus_reply_method_errorf(message, SD_BUS_ERROR_FAILED,
                                            "unreviewed or consumed plan");
    ++state.applies;
    return ::sd_bus_reply_method_return(message, "s", "adopted");
}
constexpr sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("PlanAdoptDesktopIntegration", "ss", "ss", plan, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ApplyDesktopIntegration", "s", "s", apply, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};
} // namespace

bool noctalia_config_tests(const std::filesystem::path& base);

int main() {
    const auto config_root = std::filesystem::current_path() / "build" / "test-data"
        / ("noctalia-config-" + std::to_string(::getpid()));
    std::filesystem::remove_all(config_root);
    const auto config_ok = noctalia_config_tests(config_root);
    std::filesystem::remove_all(config_root);
    if (!config_ok)
        return 1;
    PrivateBus daemon;
    if (!daemon.start())
        return 1;
    sd_bus* bus = nullptr;
    Service state;
    if (::sd_bus_open_system(&bus) < 0)
        return 1;
    if (::sd_bus_add_object_vtable(bus, nullptr, "/io/github/smile2unlock/Deployment1",
                                   "io.github.smile2unlock.Deployment1", vtable, &state) < 0 ||
        ::sd_bus_request_name(bus, "io.github.smile2unlock.Deployment1", 0) < 0) {
        ::sd_bus_unref(bus);
        return 1;
    }
    auto worker = std::jthread([bus](std::stop_token stop) {
        while (!stop.stop_requested()) {
            const auto result = ::sd_bus_process(bus, nullptr);
            if (result < 0)
                break;
            if (result == 0)
                ::sd_bus_wait(bus, 100000);
        }
    });
    // A temporary client is deliberately destroyed before confirmation.
    auto review = su::deploy::DeploymentClient{}.plan_adoption("greetd");
    auto ok = review && review->id == "reviewed-plan" &&
              review->description == "{\"operation\":\"adopt\"}" && state.plans == 1 &&
              state.applies == 0;
    if (ok) {
        const auto result = review->apply();
        ok = result && *result == "adopted" && state.applies == 1 && !review->apply();
    }
    ok = ok && !su::deploy::DeploymentClient{}.plan_adoption("greetd", "invalid");
    worker.request_stop();
    worker.join();
    ::sd_bus_unref(bus);
    if (!ok)
        std::cerr << "FAIL: adoption review/confirm D-Bus session\n";
    return ok ? 0 : 1;
}
