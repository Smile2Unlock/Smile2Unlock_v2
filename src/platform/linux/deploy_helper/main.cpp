#include "platform/linux/deploy/deployment.h"

#include <systemd/sd-bus.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::string_view kBusName = "io.github.smile2unlock.Deployment1";
constexpr std::string_view kObjectPath = "/io/github/smile2unlock/Deployment1";
constexpr std::string_view kInterface = "io.github.smile2unlock.Deployment1";
constexpr std::string_view kInitializeAction =
    "io.github.smile2unlock.deployment.initialize-runtime";
constexpr std::string_view kConfigureAction =
    "io.github.smile2unlock.deployment.configure-pam";
constexpr std::string_view kRollbackAction =
    "io.github.smile2unlock.deployment.rollback-pam";

struct ServiceState {
    struct PendingPlan {
        su::deploy::PamPlan plan;
        std::string owner;
        std::chrono::steady_clock::time_point expires_at;
    };

    std::unordered_map<std::string, PendingPlan> plans;
    std::atomic<std::uint64_t> next_plan{1};
};

constexpr auto kPlanLifetime = std::chrono::minutes{2};
constexpr auto kMaximumPendingPlans = std::size_t{64};

std::string error_message(const sd_bus_error& error, std::string_view fallback) {
    return error.message != nullptr ? std::string(error.message) : std::string(fallback);
}

int reply_error(sd_bus_message* message, std::string_view detail) {
    return ::sd_bus_reply_method_errorf(
        message, SD_BUS_ERROR_FAILED, "%.*s", static_cast<int>(detail.size()), detail.data());
}

bool trusted_executable(const std::filesystem::path& path) {
    struct stat status {};
    return ::lstat(path.c_str(), &status) == 0
        && S_ISREG(status.st_mode)
        && status.st_uid == 0
        && (status.st_mode & (S_IWGRP | S_IWOTH)) == 0
        && (status.st_mode & S_IXUSR) != 0;
}

std::optional<std::filesystem::path> systemctl_path() {
    for (const auto path : {"/usr/bin/systemctl", "/bin/systemctl"}) {
        if (trusted_executable(path)) {
            return std::filesystem::path(path);
        }
    }
    return std::nullopt;
}

std::expected<void, std::string> run_fixed(
    const std::filesystem::path& executable,
    std::span<const std::string_view> arguments) {
    if (!trusted_executable(executable)) {
        return std::unexpected("trusted executable is missing or unsafe: " + executable.string());
    }
    const auto child = ::fork();
    if (child < 0) {
        return std::unexpected("failed to fork deployment operation");
    }
    if (child == 0) {
        auto owned = std::vector<std::string>{executable.string()};
        owned.reserve(arguments.size() + 1);
        for (const auto argument : arguments) {
            owned.emplace_back(argument);
        }
        auto argv = std::vector<char*>{};
        argv.reserve(owned.size() + 1);
        for (auto& argument : owned) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        auto path_environment = std::string("PATH=/usr/sbin:/usr/bin:/sbin:/bin");
        auto language_environment = std::string("LANG=C");
        auto environment = std::array{
            path_environment.data(), language_environment.data(), static_cast<char*>(nullptr)};
        ::execve(executable.c_str(), argv.data(), environment.data());
        _exit(127);
    }
    auto status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return std::unexpected("failed to wait for deployment operation");
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected(std::format(
            "deployment operation failed with status {}",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1));
    }
    return {};
}

std::expected<void, std::string> check_authorization(
    sd_bus_message* call,
    std::string_view action) {
    const auto* sender = ::sd_bus_message_get_sender(call);
    auto* bus = ::sd_bus_message_get_bus(call);
    if (sender == nullptr || bus == nullptr) {
        return std::unexpected("cannot identify D-Bus caller");
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* request = nullptr;
    sd_bus_message* reply = nullptr;
    auto result = ::sd_bus_message_new_method_call(
        bus,
        &request,
        "org.freedesktop.PolicyKit1",
        "/org/freedesktop/PolicyKit1/Authority",
        "org.freedesktop.PolicyKit1.Authority",
        "CheckAuthorization");
    if (result < 0) {
        return std::unexpected("failed to create Polkit request");
    }
    const auto cleanup = [&] {
        ::sd_bus_message_unref(request);
        ::sd_bus_message_unref(reply);
        ::sd_bus_error_free(&error);
    };

    result = ::sd_bus_message_open_container(request, 'r', "sa{sv}");
    result = result < 0 ? result : ::sd_bus_message_append(request, "s", "system-bus-name");
    result = result < 0 ? result : ::sd_bus_message_open_container(request, 'a', "{sv}");
    result = result < 0 ? result : ::sd_bus_message_open_container(request, 'e', "sv");
    result = result < 0 ? result : ::sd_bus_message_append(request, "s", "name");
    result = result < 0 ? result : ::sd_bus_message_open_container(request, 'v', "s");
    result = result < 0 ? result : ::sd_bus_message_append(request, "s", sender);
    result = result < 0 ? result : ::sd_bus_message_close_container(request);
    result = result < 0 ? result : ::sd_bus_message_close_container(request);
    result = result < 0 ? result : ::sd_bus_message_close_container(request);
    result = result < 0 ? result : ::sd_bus_message_close_container(request);
    result = result < 0 ? result : ::sd_bus_message_append(
        request, "s", std::string(action).c_str());
    result = result < 0 ? result : ::sd_bus_message_open_container(request, 'a', "{ss}");
    result = result < 0 ? result : ::sd_bus_message_close_container(request);
    constexpr auto allow_user_interaction = std::uint32_t{1};
    result = result < 0 ? result : ::sd_bus_message_append(
        request, "us", allow_user_interaction, "");
    if (result < 0) {
        cleanup();
        return std::unexpected("failed to encode Polkit request");
    }
    result = ::sd_bus_call(bus, request, 0, &error, &reply);
    if (result < 0) {
        const auto message = error_message(error, "Polkit authorization failed");
        cleanup();
        return std::unexpected(message);
    }
    auto authorized = 0;
    auto challenge = 0;
    result = ::sd_bus_message_enter_container(reply, 'r', "bba{ss}");
    result = result < 0 ? result : ::sd_bus_message_read(reply, "bb", &authorized, &challenge);
    cleanup();
    if (result < 0 || authorized == 0) {
        return std::unexpected(challenge != 0
            ? "administrator authorization was not completed"
            : "administrator authorization was denied");
    }
    return {};
}

int inspect(sd_bus_message* message, void*, sd_bus_error*) {
    const auto snapshot = su::deploy::inspect_deployment();
    if (!snapshot) {
        return reply_error(message, snapshot.error());
    }
    const auto payload = su::deploy::snapshot_json(*snapshot);
    return ::sd_bus_reply_method_return(message, "s", payload.c_str());
}

int plan_integration(sd_bus_message* message, void* userdata, sd_bus_error*) {
    const char* target_text = nullptr;
    auto wallet_token = 0;
    if (::sd_bus_message_read(message, "sb", &target_text, &wallet_token) < 0
        || target_text == nullptr) {
        return reply_error(message, "invalid desktop integration request");
    }
    if (wallet_token != 0) {
        return reply_error(
            message,
            "wallet token deployment requires verified display-manager keyring inheritance");
    }
    const auto target = su::deploy::target_from_id(target_text);
    if (!target) {
        return reply_error(message, "unsupported desktop integration target");
    }
    if (*target == su::deploy::TargetKind::kGreetd) {
        return reply_error(message, "desktop integration target has not passed acceptance testing");
    }
    auto plan = su::deploy::plan_pam_integration("/", *target, wallet_token != 0);
    if (!plan) {
        return reply_error(message, plan.error());
    }
    auto* state = static_cast<ServiceState*>(userdata);
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(state->plans, [now](const auto& entry) {
        return entry.second.expires_at <= now;
    });
    if (state->plans.size() >= kMaximumPendingPlans) {
        return reply_error(message, "too many pending deployment plans");
    }
    const auto* sender = ::sd_bus_message_get_sender(message);
    if (sender == nullptr) {
        return reply_error(message, "cannot identify D-Bus caller");
    }
    const auto id = std::format(
        "{}-{}-{}",
        ::getpid(),
        std::chrono::steady_clock::now().time_since_epoch().count(),
        state->next_plan.fetch_add(1, std::memory_order_relaxed));
    const auto payload = su::deploy::plan_json(*plan);
    state->plans.insert_or_assign(id, ServiceState::PendingPlan{
        .plan = std::move(*plan),
        .owner = sender,
        .expires_at = now + kPlanLifetime,
    });
    return ::sd_bus_reply_method_return(message, "ss", id.c_str(), payload.c_str());
}

int apply_integration(sd_bus_message* message, void* userdata, sd_bus_error*) {
    const char* plan_id = nullptr;
    if (::sd_bus_message_read(message, "s", &plan_id) < 0 || plan_id == nullptr) {
        return reply_error(message, "invalid deployment plan identifier");
    }
    if (const auto authorized = check_authorization(message, kConfigureAction); !authorized) {
        return reply_error(message, authorized.error());
    }
    auto* state = static_cast<ServiceState*>(userdata);
    const auto found = state->plans.find(plan_id);
    const auto* sender = ::sd_bus_message_get_sender(message);
    if (found == state->plans.end() || sender == nullptr
        || found->second.owner != sender
        || found->second.expires_at <= std::chrono::steady_clock::now()) {
        if (found != state->plans.end()) {
            state->plans.erase(found);
        }
        return reply_error(message, "deployment plan expired or does not exist");
    }
    const auto applied = su::deploy::apply_pam_plan("/", found->second.plan);
    state->plans.erase(found);
    if (!applied) {
        return reply_error(message, applied.error());
    }
    const auto snapshot = su::deploy::inspect_deployment();
    if (!snapshot) {
        return reply_error(message, snapshot.error());
    }
    const auto payload = su::deploy::snapshot_json(*snapshot);
    return ::sd_bus_reply_method_return(message, "s", payload.c_str());
}

int rollback_integration(sd_bus_message* message, void*, sd_bus_error*) {
    const char* target_text = nullptr;
    if (::sd_bus_message_read(message, "s", &target_text) < 0 || target_text == nullptr) {
        return reply_error(message, "invalid rollback target");
    }
    const auto target = su::deploy::target_from_id(target_text);
    if (!target) {
        return reply_error(message, "unsupported rollback target");
    }
    if (const auto authorized = check_authorization(message, kRollbackAction); !authorized) {
        return reply_error(message, authorized.error());
    }
    const auto rolled_back = su::deploy::rollback_pam_integration("/", *target);
    if (!rolled_back) {
        return reply_error(message, rolled_back.error());
    }
    const auto snapshot = su::deploy::inspect_deployment();
    if (!snapshot) {
        return reply_error(message, snapshot.error());
    }
    const auto payload = su::deploy::snapshot_json(*snapshot);
    return ::sd_bus_reply_method_return(message, "s", payload.c_str());
}

int initialize_runtime(sd_bus_message* message, void*, sd_bus_error*) {
    if (const auto authorized = check_authorization(message, kInitializeAction); !authorized) {
        return reply_error(message, authorized.error());
    }
    constexpr auto setup = std::string_view{"/usr/libexec/smile2unlock/setup-storage-key"};
    if (const auto initialized = run_fixed(setup, {}); !initialized) {
        return reply_error(message, initialized.error());
    }
    const auto systemctl = systemctl_path();
    if (!systemctl) {
        return reply_error(message, "trusted systemctl executable is unavailable");
    }
    constexpr auto daemon_reload = std::array{std::string_view{"daemon-reload"}};
    if (const auto reloaded = run_fixed(*systemctl, daemon_reload); !reloaded) {
        return reply_error(message, reloaded.error());
    }
    constexpr auto enable = std::array{
        std::string_view{"enable"}, std::string_view{"--now"},
        std::string_view{"su-authd.service"}};
    if (const auto enabled = run_fixed(*systemctl, enable); !enabled) {
        return reply_error(message, enabled.error());
    }
    return ::sd_bus_reply_method_return(message, "s", "runtime initialized");
}

int configure_dms(sd_bus_message* message, void*, sd_bus_error*) {
    auto enabled = 0;
    if (::sd_bus_message_read(message, "b", &enabled) < 0) {
        return reply_error(message, "invalid DMS integration request");
    }
    if (const auto authorized = check_authorization(
            message, enabled != 0 ? kConfigureAction : kRollbackAction); !authorized) {
        return reply_error(message, authorized.error());
    }
    constexpr auto installer = std::string_view{"/usr/libexec/smile2unlock/install-dms-lock"};
    const auto operation = enabled != 0
        ? std::array{std::string_view{"--install"}}
        : std::array{std::string_view{"--remove"}};
    if (const auto configured = run_fixed(installer, operation); !configured) {
        return reply_error(message, configured.error());
    }
    return ::sd_bus_reply_method_return(
        message, "s", enabled != 0 ? "DMS PAM service installed" : "DMS PAM service removed");
}

constexpr auto kVtable = std::to_array<sd_bus_vtable>({
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Inspect", "", "s", inspect, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PlanDesktopIntegration", "sb", "ss", plan_integration,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ApplyDesktopIntegration", "s", "s", apply_integration,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RollbackDesktopIntegration", "s", "s", rollback_integration,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("InitializeRuntime", "", "s", initialize_runtime,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ConfigureDms", "b", "s", configure_dms,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
});

int run_service() {
    if (::geteuid() != 0) {
        std::cerr << "su_deploy_helper must be activated as root\n";
        return 1;
    }
    sd_bus* bus = nullptr;
    sd_bus_slot* slot = nullptr;
    if (::sd_bus_open_system(&bus) < 0) {
        std::cerr << "failed to open the system bus\n";
        return 1;
    }
    auto state = ServiceState{};
    auto result = ::sd_bus_add_object_vtable(
        bus,
        &slot,
        kObjectPath.data(),
        kInterface.data(),
        kVtable.data(),
        &state);
    if (result >= 0) {
        result = ::sd_bus_request_name(bus, kBusName.data(), 0);
    }
    while (result >= 0) {
        result = ::sd_bus_process(bus, nullptr);
        if (result > 0) {
            continue;
        }
        if (result == 0) {
            result = ::sd_bus_wait(bus, UINT64_MAX);
        }
    }
    ::sd_bus_slot_unref(slot);
    ::sd_bus_unref(bus);
    return result < 0 ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "su_deploy_helper 1\n";
        return 0;
    }
    if (argc != 1) {
        std::cerr << "usage: su_deploy_helper [--version]\n";
        return 64;
    }
    return run_service();
}
