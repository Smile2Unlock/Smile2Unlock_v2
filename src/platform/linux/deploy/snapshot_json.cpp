#include "platform/linux/deploy/deployment.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <optional>

namespace su::deploy {
namespace {

using nlohmann::json;

constexpr auto kSnapshotVersion = 1;
constexpr std::array kRoles{TargetRole::kLogin, TargetRole::kLock, TargetRole::kLoginAndLock,
                            TargetRole::kPrivilege};
constexpr std::array kStates{TargetState::kAbsent,   TargetState::kSupported,
                             TargetState::kManaged,  TargetState::kExternal,
                             TargetState::kConflict, TargetState::kUnsupported};

template <typename Enum, std::size_t N, typename Id>
std::optional<Enum> enum_from_id(const std::array<Enum, N>& values, Id id_of, std::string_view id) {
    const auto found = std::ranges::find_if(values, [&](Enum value) { return id_of(value) == id; });
    return found == values.end() ? std::nullopt : std::optional<Enum>{*found};
}

std::string text_or_empty(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

bool flag_or_false(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_boolean() && found->get<bool>();
}

std::expected<TargetStatus, std::string> parse_target(const json& entry) {
    if (!entry.is_object()) {
        return std::unexpected("snapshot target is not an object");
    }
    const auto kind = target_from_id(text_or_empty(entry, "id"));
    const auto role = enum_from_id(kRoles, target_role_id, text_or_empty(entry, "role"));
    const auto state = enum_from_id(kStates, target_state_id, text_or_empty(entry, "state"));
    if (!kind || !role || !state) {
        return std::unexpected("snapshot target has an unknown id, role or state");
    }
    // An unrecognised authtok source degrades to none rather than failing the
    // whole snapshot: it only narrows what the UI offers.
    const auto source = authtok_source_from_id(text_or_empty(entry, "authtok_source"));
    auto status = TargetStatus{
        .kind = *kind,
        .role = *role,
        .state = *state,
        .service = text_or_empty(entry, "service"),
        .effective_path = text_or_empty(entry, "path"),
        .password_fallback = flag_or_false(entry, "password_fallback"),
        .wallet_modules = flag_or_false(entry, "wallet_modules"),
        .wallet_token_enabled = flag_or_false(entry, "wallet_token_enabled"),
        .authtok_source = source.value_or(AuthtokSource::kNone),
        .sealed_available = flag_or_false(entry, "sealed_available"),
        .detail = text_or_empty(entry, "detail"),
        .note = text_or_empty(entry, "note"),
        .adoptable = flag_or_false(entry, "adoptable"),
    };
    if (entry.contains("adoption_unavailable_reason")) {
        status.adoption_unavailable_reason = text_or_empty(entry, "adoption_unavailable_reason");
    }
    return status;
}

} // namespace

std::string snapshot_json(const DeploymentSnapshot& snapshot) {
    auto targets = json::array();
    for (const auto& target : snapshot.targets) {
        targets.push_back({
            {"id", target_id(target.kind)},
            {"service", target.service},
            {"role", target_role_id(target.role)},
            {"state", target_state_id(target.state)},
            {"path", target.effective_path.string()},
            {"password_fallback", target.password_fallback},
            {"wallet_modules", target.wallet_modules},
            {"wallet_token_enabled", target.wallet_token_enabled},
            {"authtok_source", authtok_source_id(target.authtok_source)},
            {"sealed_available", target.sealed_available},
            {"detail", target.detail},
            // A translation key the GUI resolves; it must survive the D-Bus hop.
            {"note", target.note},
            {"adoptable", target.adoptable},
            {"adoption_unavailable_reason", target.adoption_unavailable_reason},
        });
    }
    return json{{"version", kSnapshotVersion}, {"targets", std::move(targets)}}.dump();
}

std::expected<DeploymentSnapshot, std::string> parse_snapshot_json(std::string_view text) {
    const auto document = json::parse(text, nullptr, false);
    if (!document.is_object() || !document.contains("targets") || !document["targets"].is_array()) {
        return std::unexpected("deployment snapshot is malformed");
    }
    auto snapshot = DeploymentSnapshot{};
    for (const auto& entry : document["targets"]) {
        auto parsed = parse_target(entry);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        snapshot.targets.push_back(std::move(*parsed));
    }
    return snapshot;
}

} // namespace su::deploy
