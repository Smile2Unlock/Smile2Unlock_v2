#include "platform/linux/deploy_client/deployment_client.h"

#include <systemd/sd-bus.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace su::deploy {

namespace {

constexpr std::string_view kBusName = "io.github.smile2unlock.Deployment1";
constexpr std::string_view kObjectPath = "/io/github/smile2unlock/Deployment1";
constexpr std::string_view kInterface = "io.github.smile2unlock.Deployment1";

std::string bus_error(const sd_bus_error& error, std::string_view fallback) {
    return error.message != nullptr ? std::string(error.message) : std::string(fallback);
}

std::expected<std::string, std::string> call_string_method(
    std::string_view method,
    const char* signature = "") {
    sd_bus* bus = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    if (::sd_bus_open_system(&bus) < 0) {
        return std::unexpected("deployment helper system bus is unavailable");
    }
    const auto result = ::sd_bus_call_method(
        bus,
        kBusName.data(),
        kObjectPath.data(),
        kInterface.data(),
        std::string(method).c_str(),
        &error,
        &reply,
        signature);
    if (result < 0) {
        const auto message = bus_error(error, "deployment helper request failed");
        ::sd_bus_message_unref(reply);
        ::sd_bus_error_free(&error);
        ::sd_bus_unref(bus);
        return std::unexpected(message);
    }
    const char* payload = nullptr;
    if (::sd_bus_message_read(reply, "s", &payload) < 0 || payload == nullptr) {
        ::sd_bus_message_unref(reply);
        ::sd_bus_error_free(&error);
        ::sd_bus_unref(bus);
        return std::unexpected("deployment helper returned an invalid response");
    }
    auto output = std::string(payload);
    ::sd_bus_message_unref(reply);
    ::sd_bus_error_free(&error);
    ::sd_bus_unref(bus);
    return output;
}

std::expected<std::string, std::string> call_target_method(
    std::string_view method,
    std::string_view target) {
    sd_bus* bus = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    if (::sd_bus_open_system(&bus) < 0) {
        return std::unexpected("deployment helper system bus is unavailable");
    }
    const auto result = ::sd_bus_call_method(
        bus,
        kBusName.data(),
        kObjectPath.data(),
        kInterface.data(),
        std::string(method).c_str(),
        &error,
        &reply,
        "s",
        std::string(target).c_str());
    if (result < 0) {
        const auto message = bus_error(error, "deployment helper request failed");
        ::sd_bus_message_unref(reply);
        ::sd_bus_error_free(&error);
        ::sd_bus_unref(bus);
        return std::unexpected(message);
    }
    const char* payload = nullptr;
    if (::sd_bus_message_read(reply, "s", &payload) < 0 || payload == nullptr) {
        ::sd_bus_message_unref(reply);
        ::sd_bus_error_free(&error);
        ::sd_bus_unref(bus);
        return std::unexpected("deployment helper returned an invalid response");
    }
    auto output = std::string(payload);
    ::sd_bus_message_unref(reply);
    ::sd_bus_error_free(&error);
    ::sd_bus_unref(bus);
    return output;
}

std::expected<std::string, std::string> configure_pam_target(
    std::string_view target,
    bool wallet_token) {
    sd_bus* bus = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    if (::sd_bus_open_system(&bus) < 0) {
        return std::unexpected("deployment helper system bus is unavailable");
    }
    auto result = ::sd_bus_call_method(
        bus,
        kBusName.data(),
        kObjectPath.data(),
        kInterface.data(),
        "PlanDesktopIntegration",
        &error,
        &reply,
        "sb",
        std::string(target).c_str(),
        wallet_token ? 1 : 0);
    if (result < 0) {
        const auto message = bus_error(error, "failed to plan desktop integration");
        ::sd_bus_message_unref(reply);
        ::sd_bus_error_free(&error);
        ::sd_bus_unref(bus);
        return std::unexpected(message);
    }
    const char* plan_id = nullptr;
    const char* plan_description = nullptr;
    if (::sd_bus_message_read(reply, "ss", &plan_id, &plan_description) < 0
        || plan_id == nullptr) {
        ::sd_bus_message_unref(reply);
        ::sd_bus_error_free(&error);
        ::sd_bus_unref(bus);
        return std::unexpected("deployment helper returned an invalid plan");
    }
    const auto owned_plan_id = std::string(plan_id);
    ::sd_bus_message_unref(reply);
    reply = nullptr;
    ::sd_bus_error_free(&error);
    error = SD_BUS_ERROR_NULL;

    result = ::sd_bus_call_method(
        bus,
        kBusName.data(),
        kObjectPath.data(),
        kInterface.data(),
        "ApplyDesktopIntegration",
        &error,
        &reply,
        "s",
        owned_plan_id.c_str());
    if (result < 0) {
        const auto message = bus_error(error, "failed to apply desktop integration");
        ::sd_bus_message_unref(reply);
        ::sd_bus_error_free(&error);
        ::sd_bus_unref(bus);
        return std::unexpected(message);
    }
    const char* payload = nullptr;
    if (::sd_bus_message_read(reply, "s", &payload) < 0 || payload == nullptr) {
        ::sd_bus_message_unref(reply);
        ::sd_bus_error_free(&error);
        ::sd_bus_unref(bus);
        return std::unexpected("deployment helper returned an invalid apply result");
    }
    auto output = std::string(payload);
    ::sd_bus_message_unref(reply);
    ::sd_bus_error_free(&error);
    ::sd_bus_unref(bus);
    return output;
}

std::optional<std::filesystem::path> dms_executable() {
    if (const auto* path = std::getenv("PATH"); path != nullptr) {
        auto directories = std::string_view(path);
        while (!directories.empty()) {
            const auto separator = directories.find(':');
            const auto directory = directories.substr(0, separator);
            if (!directory.empty()) {
                const auto candidate = std::filesystem::path(directory) / "dms";
                struct stat status {};
                if (::stat(candidate.c_str(), &status) == 0
                    && S_ISREG(status.st_mode) && (status.st_mode & S_IXUSR) != 0) {
                    return candidate;
                }
            }
            if (separator == std::string_view::npos) {
                break;
            }
            directories.remove_prefix(separator + 1);
        }
    }
    return std::nullopt;
}

std::expected<void, std::string> run_dms(std::span<const std::string_view> arguments) {
    const auto executable = dms_executable();
    if (!executable) {
        return std::unexpected("DMS command-line API is unavailable");
    }
    const auto child = ::fork();
    if (child < 0) {
        return std::unexpected("failed to start DMS command-line API");
    }
    if (child == 0) {
        auto owned = std::vector<std::string>{executable->string()};
        for (const auto argument : arguments) {
            owned.emplace_back(argument);
        }
        auto argv = std::vector<char*>{};
        for (auto& argument : owned) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        ::execv(executable->c_str(), argv.data());
        _exit(127);
    }
    auto status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected("DMS rejected the lock-screen configuration");
    }
    return {};
}

std::expected<std::string, std::string> configure_dms(bool enabled) {
    if (!enabled) {
        constexpr auto clear = std::array{
            std::string_view{"ipc"}, std::string_view{"call"},
            std::string_view{"settings"}, std::string_view{"set"},
            std::string_view{"lockPamPath"}, std::string_view{""}};
        if (const auto cleared = run_dms(clear); !cleared) {
            return std::unexpected(cleared.error());
        }
    }
    sd_bus* bus = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    if (::sd_bus_open_system(&bus) < 0) {
        return std::unexpected("deployment helper system bus is unavailable");
    }
    const auto result = ::sd_bus_call_method(
        bus,
        kBusName.data(),
        kObjectPath.data(),
        kInterface.data(),
        "ConfigureDms",
        &error,
        &reply,
        "b",
        enabled ? 1 : 0);
    if (result < 0) {
        const auto message = bus_error(error, "failed to configure DMS PAM service");
        ::sd_bus_message_unref(reply);
        ::sd_bus_error_free(&error);
        ::sd_bus_unref(bus);
        return std::unexpected(message);
    }
    const char* payload = nullptr;
    (void)::sd_bus_message_read(reply, "s", &payload);
    ::sd_bus_message_unref(reply);
    ::sd_bus_error_free(&error);
    ::sd_bus_unref(bus);
    if (enabled) {
        constexpr auto validate = std::array{
            std::string_view{"auth"}, std::string_view{"validate"},
            std::string_view{"--path"},
            std::string_view{"/etc/pam.d/dankshell-smile2unlock"},
            std::string_view{"--json"}};
        if (const auto checked = run_dms(validate); !checked) {
            return std::unexpected(checked.error());
        }
        constexpr auto select = std::array{
            std::string_view{"ipc"}, std::string_view{"call"},
            std::string_view{"settings"}, std::string_view{"set"},
            std::string_view{"lockPamPath"},
            std::string_view{"/etc/pam.d/dankshell-smile2unlock"}};
        if (const auto selected = run_dms(select); !selected) {
            return std::unexpected(selected.error());
        }
    }
    return enabled ? "DMS lock-screen authentication configured"
                   : "DMS lock-screen authentication removed";
}

} // namespace

bool DeploymentClient::dms_available() const {
    return dms_executable().has_value();
}

std::expected<std::string, std::string> DeploymentClient::inspect() const {
    auto payload = call_string_method("Inspect");
    if (!payload) {
        return payload;
    }
    const auto parsed = nlohmann::json::parse(*payload, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || parsed.value("version", 0) != 1) {
        return std::unexpected("deployment helper protocol is incompatible");
    }
    return payload;
}

std::expected<std::string, std::string> DeploymentClient::initialize_runtime() const {
    return call_string_method("InitializeRuntime");
}

std::expected<std::string, std::string> DeploymentClient::configure_target(
    std::string_view target,
    bool wallet_token) const {
    return target == "dms" ? configure_dms(true)
                           : configure_pam_target(target, wallet_token);
}

std::expected<std::string, std::string> DeploymentClient::rollback_target(
    std::string_view target) const {
    return target == "dms" ? configure_dms(false)
                           : call_target_method("RollbackDesktopIntegration", target);
}

} // namespace su::deploy
