#include "platform/linux/deploy/deployment.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace su::deploy {

namespace {

using nlohmann::json;

constexpr std::string_view kManagedMarker = "# Managed by Smile2Unlock deployment helper.";
constexpr std::string_view kPamModule = "pam_smile2unlock.so";
constexpr auto kJournalVersion = 2;
constexpr auto kTransformationVersion = 2;
#ifndef SU_VERSION_STR
#define SU_VERSION_STR "0.0.0"
#endif
constexpr std::string_view kPackageVersion = SU_VERSION_STR;

struct TargetDefinition {
    TargetKind kind;
    TargetRole role;
    std::string_view id;
    std::string_view service;
};

constexpr auto kTargets = std::array{
    TargetDefinition{TargetKind::kDms, TargetRole::kLock, "dms", "dankshell-smile2unlock"},
    TargetDefinition{TargetKind::kKscreenlocker, TargetRole::kLock, "kscreenlocker", "kde"},
    TargetDefinition{TargetKind::kPlasmaLogin, TargetRole::kLogin, "plasma-login", "plasmalogin"},
    TargetDefinition{TargetKind::kGdm, TargetRole::kLoginAndLock, "gdm", "gdm-password"},
    TargetDefinition{TargetKind::kSddm, TargetRole::kLogin, "sddm", "sddm"},
    TargetDefinition{TargetKind::kGreetd, TargetRole::kLogin, "greetd", "greetd"},
};

struct ParsedLine {
    std::string raw;
    std::vector<std::string> fields;
    bool comment_or_empty = false;
};

struct AuthAnchor {
    std::size_t line = 0;
    std::string service;
};

const TargetDefinition& definition(TargetKind kind) {
    return *std::ranges::find(kTargets, kind, &TargetDefinition::kind);
}

std::filesystem::path rooted(
    const std::filesystem::path& root,
    const std::filesystem::path& absolute) {
    return root / absolute.relative_path();
}

std::expected<std::string, std::string> read_regular_file(const std::filesystem::path& path) {
    auto status_error = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error || !std::filesystem::is_regular_file(status)) {
        return std::unexpected("not a regular file: " + path.string());
    }
    const auto path_text = path.string();
    if (path_text.starts_with("/etc/pam.d/")
        || path_text.starts_with("/usr/lib/pam.d/")
        || path_text == "/var/lib/smile2unlock/deployment.json") {
        struct stat metadata {};
        if (::lstat(path.c_str(), &metadata) != 0
            || metadata.st_uid != 0
            || (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            return std::unexpected("unsafe ownership or mode: " + path.string());
        }
    }
    auto input = std::ifstream(path, std::ios::binary);
    if (!input) {
        return std::unexpected("failed to read " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input), {});
}

std::string fingerprint(std::string_view content) {
    auto value = std::uint64_t{14695981039346656037ULL};
    for (const auto byte : content) {
        value ^= static_cast<unsigned char>(byte);
        value *= 1099511628211ULL;
    }
    return std::format("{:016x}", value);
}

std::vector<std::string> pam_fields(std::string_view line) {
    auto fields = std::vector<std::string>{};
    auto index = std::size_t{0};
    while (index < line.size()) {
        while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index]))) {
            ++index;
        }
        if (index == line.size() || line[index] == '#') {
            break;
        }
        const auto start = index;
        if (line[index] == '[') {
            while (index < line.size() && line[index] != ']') {
                ++index;
            }
            if (index < line.size()) {
                ++index;
            }
        } else {
            while (index < line.size()
                   && !std::isspace(static_cast<unsigned char>(line[index]))) {
                ++index;
            }
        }
        fields.emplace_back(line.substr(start, index - start));
    }
    return fields;
}

std::vector<ParsedLine> parse_lines(std::string_view content) {
    auto lines = std::vector<ParsedLine>{};
    auto stream = std::istringstream(std::string(content));
    for (auto line = std::string{}; std::getline(stream, line);) {
        auto fields = pam_fields(line);
        const auto first = line.find_first_not_of(" \t");
        lines.push_back(ParsedLine{
            .raw = std::move(line),
            .fields = std::move(fields),
            .comment_or_empty = first == std::string::npos || line[first] == '#',
        });
    }
    return lines;
}

bool password_service(std::string_view service) {
    constexpr auto services = std::array{
        std::string_view{"system-login"},
        std::string_view{"system-local-login"},
        std::string_view{"password-auth"},
        std::string_view{"common-auth"},
        std::string_view{"common-auth-pc"},
    };
    return std::ranges::find(services, service) != services.end();
}

std::optional<AuthAnchor> auth_anchor(std::span<const ParsedLine> lines) {
    for (auto index = std::size_t{0}; index < lines.size(); ++index) {
        const auto& fields = lines[index].fields;
        if (fields.size() == 2 && fields[0] == "@include" && password_service(fields[1])) {
            return AuthAnchor{.line = index, .service = fields[1]};
        }
        if (fields.size() >= 3 && fields[0] == "auth"
            && (fields[1] == "include" || fields[1] == "substack")
            && password_service(fields[2])) {
            return AuthAnchor{.line = index, .service = fields[2]};
        }
    }
    return std::nullopt;
}

bool contains_module(std::span<const ParsedLine> lines, std::string_view module) {
    return std::ranges::any_of(lines, [module](const auto& line) {
        return std::ranges::find(line.fields, module) != line.fields.end();
    });
}

bool references_substack(std::span<const ParsedLine> lines, std::string_view service) {
    return std::ranges::any_of(lines, [service](const auto& line) {
        return line.fields.size() >= 3
            && line.fields[0] == "auth"
            && line.fields[1] == "substack"
            && line.fields[2] == service;
    });
}

bool references_auth_service(std::span<const ParsedLine> lines, std::string_view service) {
    return std::ranges::any_of(lines, [service](const auto& line) {
        return (line.fields.size() == 2
                && line.fields[0] == "@include"
                && line.fields[1] == service)
            || (line.fields.size() >= 3
                && line.fields[0] == "auth"
                && (line.fields[1] == "include" || line.fields[1] == "substack")
                && line.fields[2] == service);
    });
}

bool contains_wallet_module(std::span<const ParsedLine> lines) {
    return contains_module(lines, "pam_kwallet5.so")
        || contains_module(lines, "pam_gnome_keyring.so");
}

std::filesystem::path pam_path(
    const std::filesystem::path& root,
    std::string_view directory,
    std::string_view service) {
    return rooted(root, std::filesystem::path(directory) / service);
}

std::optional<std::filesystem::path> effective_service_path(
    const std::filesystem::path& root,
    std::string_view service) {
    for (const auto directory : {"/etc/pam.d", "/usr/lib/pam.d"}) {
        const auto candidate = pam_path(root, directory, service);
        auto error = std::error_code{};
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string child_service_name(TargetKind target) {
    return "smile2unlock-" + std::string(target_id(target)) + "-auth";
}

std::filesystem::path journal_path(const std::filesystem::path& root) {
    return rooted(root, "/var/lib/smile2unlock/deployment.json");
}

std::filesystem::path backup_directory(const std::filesystem::path& root) {
    return rooted(root, "/var/lib/smile2unlock/pam-backups");
}

std::optional<std::array<unsigned, 3>> parse_version(std::string_view text) {
    auto result = std::array<unsigned, 3>{};
    for (auto index = std::size_t{0}; index < result.size(); ++index) {
        const auto separator = text.find('.');
        const auto component = text.substr(0, separator);
        const auto parsed = std::from_chars(
            component.data(), component.data() + component.size(), result[index]);
        if (component.empty() || parsed.ec != std::errc{}
            || parsed.ptr != component.data() + component.size()) {
            return std::nullopt;
        }
        if (index + 1 == result.size()) {
            if (separator != std::string_view::npos) {
                return std::nullopt;
            }
        } else {
            if (separator == std::string_view::npos) {
                return std::nullopt;
            }
            text.remove_prefix(separator + 1);
        }
    }
    return result;
}

std::expected<void, std::string> reject_downgrade(const json& journal) {
    if (!journal.contains("targets") || journal["targets"].empty()) {
        return {};
    }
    const auto recorded_text = journal.value("package_version", "0.0.0");
    const auto recorded = parse_version(recorded_text);
    const auto current = parse_version(kPackageVersion);
    if (!recorded || !current) {
        return std::unexpected("deployment journal contains an invalid package version");
    }
    if (*recorded > *current) {
        return std::unexpected(std::format(
            "refusing package downgrade from {} to {} while PAM integration is managed",
            recorded_text, kPackageVersion));
    }
    return {};
}

std::string join_lines(std::span<const ParsedLine> lines) {
    auto output = std::string{};
    for (const auto& line : lines) {
        output += line.raw;
        output += '\n';
    }
    return output;
}

std::string child_content(
    std::string_view password_stack,
    bool wallet_token) {
    auto output = std::string{
        "#%PAM-1.0\n"
        "# Managed by Smile2Unlock deployment helper.\n"
        "# Face failures continue to the distribution password stack.\n"};
    if (wallet_token) {
        output += "auth [success=2 default=ignore] pam_smile2unlock.so socket=/run/smile2unlock/control.sock\n";
        output += std::format("auth substack {}\n", password_stack);
        output += "auth [success=2 default=ignore] pam_permit.so\n";
        output += "-auth optional pam_systemd_loadkey.so\n";
        output += "auth required pam_permit.so\n";
    } else {
        output += "auth sufficient pam_smile2unlock.so socket=/run/smile2unlock/control.sock\n";
        output += std::format("auth substack {}\n", password_stack);
    }
    return output;
}

std::expected<void, std::string> write_all(int fd, std::string_view content) {
    auto remaining = content;
    while (!remaining.empty()) {
        const auto written = ::write(fd, remaining.data(), remaining.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected("write failed: " + std::string(std::strerror(errno)));
        }
        remaining.remove_prefix(static_cast<std::size_t>(written));
    }
    return {};
}

std::expected<void, std::string> atomic_write(
    const std::filesystem::path& path,
    std::string_view content,
    mode_t mode) {
    auto error = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return std::unexpected("failed to create " + path.parent_path().string());
    }
    const auto directory_fd = ::open(
        path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        return std::unexpected("failed to open directory " + path.parent_path().string());
    }
    const auto close_directory = std::unique_ptr<int, void (*)(int*)>{
        new int(directory_fd), [](int* fd) { ::close(*fd); delete fd; }};
    const auto temporary_name = std::format(".{}.{}.tmp", path.filename().string(), ::getpid());
    const auto temporary_fd = ::openat(
        directory_fd,
        temporary_name.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
        mode);
    if (temporary_fd < 0) {
        return std::unexpected("failed to create temporary file for " + path.string());
    }
    const auto cleanup = std::unique_ptr<int, std::function<void(int*)>>{
        new int(temporary_fd),
        [directory_fd, temporary_name](int* fd) {
            ::close(*fd);
            ::unlinkat(directory_fd, temporary_name.c_str(), 0);
            delete fd;
        }};
    if (const auto written = write_all(temporary_fd, content); !written) {
        return written;
    }
    if (::fchmod(temporary_fd, mode) != 0 || ::fsync(temporary_fd) != 0) {
        return std::unexpected("failed to sync temporary file for " + path.string());
    }
    if (::renameat(directory_fd, temporary_name.c_str(), directory_fd, path.filename().c_str()) != 0) {
        return std::unexpected("failed to replace " + path.string());
    }
    (void)::fsync(directory_fd);
    ::close(*cleanup);
    *cleanup = -1;
    return {};
}

std::expected<json, std::string> load_journal(const std::filesystem::path& root) {
    const auto path = journal_path(root);
    auto error = std::error_code{};
    if (!std::filesystem::exists(path, error)) {
        return json{
            {"version", kJournalVersion},
            {"package_version", kPackageVersion},
            {"transformation_version", kTransformationVersion},
            {"targets", json::object()},
        };
    }
    const auto content = read_regular_file(path);
    if (!content) {
        return std::unexpected(content.error());
    }
    auto parsed = json::parse(*content, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()
        || (parsed.value("version", 0) != 1
            && parsed.value("version", 0) != kJournalVersion)
        || !parsed.contains("targets")
        || !parsed["targets"].is_object()) {
        return std::unexpected("invalid deployment journal");
    }
    if (parsed.value("version", 0) == 1) {
        parsed["version"] = kJournalVersion;
        parsed["package_version"] = "0.0.0";
        parsed["transformation_version"] = 1;
        for (auto& [_, record] : parsed["targets"].items()) {
            if (record.is_object()) {
                record["state"] = "active";
                const auto parent = read_regular_file(record.value("parent_backup", ""));
                const auto child = read_regular_file(record.value("child_backup", ""));
                record["source_fingerprint"] = parent ? fingerprint(*parent) : "";
                record["child_source_fingerprint"] = child ? fingerprint(*child) : "";
                record["package_version"] = "0.0.0";
                record["transformation_version"] = 1;
            }
        }
    }
    return parsed;
}

std::expected<void, std::string> save_journal(
    const std::filesystem::path& root,
    const json& journal) {
    return atomic_write(journal_path(root), journal.dump(2) + '\n', 0600);
}

std::expected<void, std::string> remove_regular_file(const std::filesystem::path& path) {
    auto error = std::error_code{};
    if (!std::filesystem::exists(path, error)) {
        return {};
    }
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        return std::unexpected("refusing to remove non-regular file " + path.string());
    }
    if (!std::filesystem::remove(path, error) || error) {
        return std::unexpected("failed to remove " + path.string());
    }
    return {};
}

TargetStatus inspect_target(
    const std::filesystem::path& root,
    const TargetDefinition& target) {
    auto status = TargetStatus{
        .kind = target.kind,
        .role = target.role,
        .state = TargetState::kAbsent,
        .service = std::string(target.service),
        .effective_path = {},
        .password_fallback = false,
        .wallet_modules = false,
        .wallet_token_enabled = false,
        .detail = {},
    };
    const auto path = effective_service_path(root, target.service);
    if (!path) {
        status.detail = "PAM service is not installed";
        return status;
    }
    status.effective_path = *path;
    const auto content = read_regular_file(*path);
    if (!content) {
        status.state = TargetState::kConflict;
        status.detail = content.error();
        return status;
    }
    const auto lines = parse_lines(*content);
    status.wallet_modules = contains_wallet_module(lines);
    status.wallet_token_enabled = contains_module(lines, "pam_systemd_loadkey.so");
    status.password_fallback = auth_anchor(lines).has_value();
    const auto child_service = child_service_name(target.kind);
    if (references_substack(lines, child_service)) {
        const auto child_path = effective_service_path(root, child_service);
        if (!child_path) {
            status.state = TargetState::kConflict;
            status.detail = "managed substack is missing";
            return status;
        }
        const auto child = read_regular_file(*child_path);
        if (!child || !child->starts_with("#%PAM-1.0\n" + std::string(kManagedMarker))) {
            status.state = TargetState::kConflict;
            status.detail = "managed substack was modified";
            return status;
        }
        const auto child_lines = parse_lines(*child);
        status.state = TargetState::kManaged;
        status.password_fallback = auth_anchor(child_lines).has_value();
        status.wallet_token_enabled = contains_module(child_lines, "pam_systemd_loadkey.so");
        status.detail = "managed by Smile2Unlock";
        return status;
    }
    if (contains_module(lines, kPamModule)) {
        status.state = target.kind == TargetKind::kDms
            ? TargetState::kManaged
            : TargetState::kExternal;
        if (target.kind == TargetKind::kDms) {
            status.password_fallback = references_auth_service(lines, "login");
        }
        status.detail = target.kind == TargetKind::kDms
            ? "dedicated DMS PAM service is installed"
            : "Smile2Unlock is configured outside the deployment helper";
        return status;
    }
    if (target.kind == TargetKind::kDms) {
        status.state = TargetState::kUnsupported;
        status.detail = "DMS service does not contain the Smile2Unlock module";
        return status;
    }
    if (status.password_fallback) {
        status.state = TargetState::kSupported;
        status.detail = "recognized password authentication stack";
    } else {
        status.state = TargetState::kUnsupported;
        status.detail = "unsupported PAM authentication layout";
    }
    return status;
}

} // namespace

std::string_view target_id(TargetKind target) {
    return definition(target).id;
}

std::string_view target_service(TargetKind target) {
    return definition(target).service;
}

std::string_view target_state_id(TargetState state) {
    switch (state) {
    case TargetState::kAbsent: return "absent";
    case TargetState::kSupported: return "supported";
    case TargetState::kManaged: return "managed";
    case TargetState::kExternal: return "external";
    case TargetState::kConflict: return "conflict";
    case TargetState::kUnsupported: return "unsupported";
    }
    std::unreachable();
}

std::optional<TargetKind> target_from_id(std::string_view id) {
    const auto found = std::ranges::find(kTargets, id, &TargetDefinition::id);
    return found == kTargets.end() ? std::nullopt : std::optional(found->kind);
}

std::expected<DeploymentSnapshot, std::string> inspect_deployment(
    const std::filesystem::path& root) {
    auto snapshot = DeploymentSnapshot{};
    snapshot.targets.reserve(kTargets.size());
    for (const auto& target : kTargets) {
        snapshot.targets.push_back(inspect_target(root, target));
    }
    return snapshot;
}

std::expected<PamPlan, std::string> plan_pam_integration(
    const std::filesystem::path& root,
    TargetKind target,
    bool wallet_token) {
    if (target == TargetKind::kDms) {
        return std::unexpected("DMS uses its dedicated PAM service installer");
    }
    const auto service = target_service(target);
    const auto source_path = effective_service_path(root, service);
    if (!source_path) {
        return std::unexpected("PAM service is not installed: " + std::string(service));
    }
    const auto source = read_regular_file(*source_path);
    if (!source) {
        return std::unexpected(source.error());
    }
    auto lines = parse_lines(*source);
    const auto child_service = child_service_name(target);
    const auto child_path = pam_path(root, "/etc/pam.d", child_service);
    const auto destination_path = pam_path(root, "/etc/pam.d", service);
    if (references_substack(lines, child_service)) {
        const auto child = read_regular_file(child_path);
        if (!child || child->find(kManagedMarker) == std::string::npos) {
            return std::unexpected("managed PAM substack is missing or modified");
        }
        const auto child_lines = parse_lines(*child);
        const auto password_anchor = auth_anchor(child_lines);
        if (!password_anchor) {
            return std::unexpected("managed PAM substack has no password fallback");
        }
        const auto journal = load_journal(root);
        if (!journal) {
            return std::unexpected(journal.error());
        }
        if (const auto compatible = reject_downgrade(*journal); !compatible) {
            return std::unexpected(compatible.error());
        }
        const auto id = std::string(target_id(target));
        if (!(*journal)["targets"].contains(id)
            || (*journal)["targets"][id].value("state", "active") != "active") {
            return std::unexpected("managed PAM files do not have an active journal record");
        }
        const auto desired_child = child_content(password_anchor->service, wallet_token);
        const auto& record = (*journal)["targets"][id];
        const auto current_transformation = record.value("transformation_version", 0)
            == kTransformationVersion;
        const auto current_package = record.value("package_version", "")
            == kPackageVersion;
        return PamPlan{
            .target = target,
            .service = std::string(service),
            .child_service = child_service,
            .source_path = *source_path,
            .destination_path = destination_path,
            .child_path = child_path,
            .source_content = *source,
            .destination_content = *source,
            .child_content = desired_child,
            .source_fingerprint = fingerprint(*source),
            .child_source_content = *child,
            .child_source_fingerprint = fingerprint(*child),
            .destination_existed = *source_path == destination_path,
            .child_existed = true,
            .wallet_token_requested = wallet_token,
            .already_managed = *child == desired_child
                && current_transformation && current_package,
        };
    }
    if (contains_module(lines, kPamModule)) {
        return std::unexpected("PAM service is configured outside the deployment helper");
    }
    const auto anchor = auth_anchor(lines);
    if (!anchor) {
        return std::unexpected("unsupported PAM authentication layout");
    }
    auto child_source = std::string{};
    auto child_source_fingerprint = std::string{};
    auto child_existed = false;
    auto error = std::error_code{};
    if (std::filesystem::exists(child_path, error)) {
        const auto existing = read_regular_file(child_path);
        if (!existing) {
            return std::unexpected(existing.error());
        }
        if (existing->find(kManagedMarker) == std::string::npos) {
            return std::unexpected("refusing to replace an external PAM substack");
        }
        child_source = *existing;
        child_source_fingerprint = fingerprint(*existing);
        child_existed = true;
    }
    lines[anchor->line].raw = std::format("auth       substack    {}", child_service);
    lines[anchor->line].fields = pam_fields(lines[anchor->line].raw);
    return PamPlan{
        .target = target,
        .service = std::string(service),
        .child_service = child_service,
        .source_path = *source_path,
        .destination_path = destination_path,
        .child_path = child_path,
        .source_content = *source,
        .destination_content = join_lines(lines),
        .child_content = child_content(anchor->service, wallet_token),
        .source_fingerprint = fingerprint(*source),
        .child_source_content = std::move(child_source),
        .child_source_fingerprint = std::move(child_source_fingerprint),
        .destination_existed = *source_path == destination_path,
        .child_existed = child_existed,
        .wallet_token_requested = wallet_token,
    };
}

std::expected<void, std::string> apply_pam_plan(
    const std::filesystem::path& root,
    const PamPlan& plan) {
    if (plan.already_managed) {
        return {};
    }
    const auto current_source = read_regular_file(plan.source_path);
    if (!current_source || *current_source != plan.source_content
        || fingerprint(*current_source) != plan.source_fingerprint) {
        return std::unexpected("PAM service changed after the plan was created");
    }
    auto error = std::error_code{};
    if (plan.source_path != plan.destination_path
        && std::filesystem::exists(plan.destination_path, error)) {
        return std::unexpected("administrator override appeared after the plan was created");
    }
    if (plan.child_existed) {
        const auto current_child = read_regular_file(plan.child_path);
        if (!current_child || *current_child != plan.child_source_content
            || fingerprint(*current_child) != plan.child_source_fingerprint) {
            return std::unexpected("PAM substack changed after the plan was created");
        }
    } else if (std::filesystem::exists(plan.child_path, error)) {
        return std::unexpected("PAM substack appeared after the plan was created");
    }

    auto journal = load_journal(root);
    if (!journal) {
        return std::unexpected(journal.error());
    }
    if (const auto compatible = reject_downgrade(*journal); !compatible) {
        return std::unexpected(compatible.error());
    }
    const auto id = std::string(target_id(plan.target));
    if ((*journal)["targets"].contains(id)) {
        auto& record = (*journal)["targets"][id];
        if (record.value("state", "active") != "active"
            || fingerprint(*current_source) != record.value("result_fingerprint", "")) {
            return std::unexpected("managed PAM target is not in a safe upgrade state");
        }
        const auto upgrade_backup = backup_directory(root) / (id + ".upgrade-child");
        if (const auto saved = atomic_write(upgrade_backup, plan.child_source_content, 0600);
            !saved) {
            return saved;
        }
        const auto old_record = record;
        record["state"] = "upgrading";
        record["upgrade_child_backup"] = upgrade_backup.string();
        record["pending_child_result_fingerprint"] = fingerprint(plan.child_content);
        if (const auto saved = save_journal(root, *journal); !saved) {
            return saved;
        }
        const auto restore_upgrade = [&]() -> std::expected<void, std::string> {
            const auto restored = atomic_write(plan.child_path, plan.child_source_content, 0644);
            (*journal)["targets"][id] = old_record;
            const auto journal_saved = save_journal(root, *journal);
            (void)remove_regular_file(upgrade_backup);
            if (!restored) return restored;
            return journal_saved;
        };
        if (const auto written = atomic_write(plan.child_path, plan.child_content, 0644);
            !written) {
            (void)restore_upgrade();
            return written;
        }
        const auto installed_child = read_regular_file(plan.child_path);
        const auto status = inspect_target(root, definition(plan.target));
        if (!installed_child || *installed_child != plan.child_content
            || status.state != TargetState::kManaged || !status.password_fallback) {
            (void)restore_upgrade();
            return std::unexpected("upgraded PAM stack failed post-write validation");
        }
        record = old_record;
        record["state"] = "active";
        record["child_result_fingerprint"] = fingerprint(plan.child_content);
        record["wallet_token"] = plan.wallet_token_requested;
        record["package_version"] = kPackageVersion;
        record["transformation_version"] = kTransformationVersion;
        (*journal)["package_version"] = kPackageVersion;
        (*journal)["transformation_version"] = kTransformationVersion;
        if (const auto saved = save_journal(root, *journal); !saved) {
            (void)restore_upgrade();
            return saved;
        }
        (void)remove_regular_file(upgrade_backup);
        return {};
    }

    const auto backup_dir = backup_directory(root);
    const auto parent_backup = backup_dir / (id + ".parent");
    const auto child_backup = backup_dir / (id + ".child");
    if (plan.destination_existed) {
        if (const auto saved = atomic_write(parent_backup, plan.source_content, 0600); !saved) {
            return saved;
        }
    }
    if (plan.child_existed) {
        if (const auto saved = atomic_write(child_backup, plan.child_source_content, 0600); !saved) {
            return saved;
        }
    }

    (*journal)["targets"][id] = {
        {"state", "applying"},
        {"service", plan.service},
        {"destination", plan.destination_path.string()},
        {"child", plan.child_path.string()},
        {"destination_existed", plan.destination_existed},
        {"child_existed", plan.child_existed},
        {"parent_backup", parent_backup.string()},
        {"child_backup", child_backup.string()},
        {"source_fingerprint", fingerprint(plan.source_content)},
        {"child_source_fingerprint", fingerprint(plan.child_source_content)},
        {"result_fingerprint", fingerprint(plan.destination_content)},
        {"child_result_fingerprint", fingerprint(plan.child_content)},
        {"wallet_token", plan.wallet_token_requested},
        {"package_version", kPackageVersion},
        {"transformation_version", kTransformationVersion},
    };
    (*journal)["package_version"] = kPackageVersion;
    (*journal)["transformation_version"] = kTransformationVersion;
    if (const auto saved = save_journal(root, *journal); !saved) {
        return saved;
    }

    const auto restore_fresh = [&]() -> std::expected<void, std::string> {
        auto first_error = std::optional<std::string>{};
        const auto parent = plan.destination_existed
            ? atomic_write(plan.destination_path, plan.source_content, 0644)
            : remove_regular_file(plan.destination_path);
        if (!parent) first_error = parent.error();
        const auto child = plan.child_existed
            ? atomic_write(plan.child_path, plan.child_source_content, 0644)
            : remove_regular_file(plan.child_path);
        if (!child && !first_error) first_error = child.error();
        if (!first_error) {
            (*journal)["targets"].erase(id);
            if (const auto saved = save_journal(root, *journal); !saved) {
                first_error = saved.error();
            }
        }
        return first_error ? std::expected<void, std::string>{std::unexpected(*first_error)}
                           : std::expected<void, std::string>{};
    };

    if (const auto written = atomic_write(plan.child_path, plan.child_content, 0644); !written) {
        (void)restore_fresh();
        return written;
    }
    if (const auto written = atomic_write(
            plan.destination_path, plan.destination_content, 0644); !written) {
        (void)restore_fresh();
        return written;
    }
    const auto installed_parent = read_regular_file(plan.destination_path);
    const auto installed_child = read_regular_file(plan.child_path);
    const auto status = inspect_target(root, definition(plan.target));
    if (!installed_parent || *installed_parent != plan.destination_content
        || !installed_child || *installed_child != plan.child_content
        || status.state != TargetState::kManaged || !status.password_fallback) {
        (void)restore_fresh();
        return std::unexpected("installed PAM stack failed post-write validation");
    }
    (*journal)["targets"][id]["state"] = "active";
    if (const auto saved = save_journal(root, *journal); !saved) {
        (void)restore_fresh();
        return saved;
    }
    return {};
}

std::expected<void, std::string> rollback_pam_integration(
    const std::filesystem::path& root,
    TargetKind target) {
    auto journal = load_journal(root);
    if (!journal) {
        return std::unexpected(journal.error());
    }
    const auto id = std::string(target_id(target));
    if (!(*journal)["targets"].contains(id)) {
        return std::unexpected("target is not managed by Smile2Unlock");
    }
    auto record = (*journal)["targets"][id];
    if (!record.is_object()) {
        return std::unexpected("deployment journal contains an invalid target record");
    }
    const auto destination = std::filesystem::path(record.value("destination", ""));
    const auto child = std::filesystem::path(record.value("child", ""));
    const auto expected_destination = pam_path(root, "/etc/pam.d", target_service(target));
    const auto expected_child = pam_path(root, "/etc/pam.d", child_service_name(target));
    const auto parent_backup = backup_directory(root) / (id + ".parent");
    const auto child_backup = backup_directory(root) / (id + ".child");
    if (destination != expected_destination || child != expected_child
        || std::filesystem::path(record.value("parent_backup", "")) != parent_backup
        || std::filesystem::path(record.value("child_backup", "")) != child_backup) {
        return std::unexpected("deployment journal contains paths outside the managed target");
    }
    const auto state = record.value("state", "active");
    if (state != "active" && state != "applying" && state != "rolling_back") {
        return std::unexpected("managed PAM target has an unfinished upgrade");
    }
    const auto path_matches = [](const std::filesystem::path& path, std::string_view expected) {
        const auto content = read_regular_file(path);
        return content && fingerprint(*content) == expected;
    };
    const auto path_absent = [](const std::filesystem::path& path) {
        auto error = std::error_code{};
        return !std::filesystem::exists(path, error) && !error;
    };
    const auto parent_result = path_matches(destination, record.value("result_fingerprint", ""));
    const auto child_result = path_matches(child, record.value("child_result_fingerprint", ""));
    const auto parent_original = record.value("destination_existed", false)
        ? path_matches(destination, record.value("source_fingerprint", ""))
        : path_absent(destination);
    const auto child_original = record.value("child_existed", false)
        ? path_matches(child, record.value("child_source_fingerprint", ""))
        : path_absent(child);
    if ((state == "active" && (!parent_result || !child_result))
        || (state != "active"
            && (!(parent_result || parent_original) || !(child_result || child_original)))) {
        return std::unexpected("managed PAM files changed after installation");
    }
    (*journal)["targets"][id]["state"] = "rolling_back";
    if (const auto saved = save_journal(root, *journal); !saved) {
        return saved;
    }

    if (record.value("destination_existed", false)) {
        const auto backup = read_regular_file(parent_backup);
        if (!backup) {
            return std::unexpected("parent PAM backup is unavailable");
        }
        if (const auto restored = atomic_write(destination, *backup, 0644); !restored) {
            return restored;
        }
    } else if (const auto removed = remove_regular_file(destination); !removed) {
        return removed;
    }
    if (record.value("child_existed", false)) {
        const auto backup = read_regular_file(child_backup);
        if (!backup) {
            return std::unexpected("child PAM backup is unavailable");
        }
        if (const auto restored = atomic_write(child, *backup, 0644); !restored) {
            return restored;
        }
    } else if (const auto removed = remove_regular_file(child); !removed) {
        return removed;
    }
    (*journal)["targets"].erase(id);
    if (const auto saved = save_journal(root, *journal); !saved) {
        return saved;
    }
    (void)remove_regular_file(parent_backup);
    (void)remove_regular_file(child_backup);
    return {};
}

std::expected<void, std::string> recover_interrupted_pam_transactions(
    const std::filesystem::path& root) {
    auto journal = load_journal(root);
    if (!journal) {
        return std::unexpected(journal.error());
    }
    auto interrupted = std::vector<TargetKind>{};
    for (const auto& [id, record] : (*journal)["targets"].items()) {
        if (!record.is_object() || record.value("state", "active") == "active") {
            continue;
        }
        const auto target = target_from_id(id);
        if (!target) {
            return std::unexpected("deployment journal contains an unknown target");
        }
        if (record.value("state", "") == "upgrading") {
            const auto child = pam_path(root, "/etc/pam.d", child_service_name(*target));
            const auto backup = backup_directory(root) / (id + ".upgrade-child");
            const auto backup_content = read_regular_file(backup);
            if (!backup_content) {
                return std::unexpected("interrupted PAM upgrade backup is unavailable");
            }
            if (fingerprint(*backup_content)
                != record.value("child_result_fingerprint", "")) {
                return std::unexpected("interrupted PAM upgrade backup failed integrity validation");
            }
            const auto current = read_regular_file(child);
            const auto current_hash = current ? fingerprint(*current) : std::string{};
            if (current_hash != record.value("child_result_fingerprint", "")
                && current_hash != record.value("pending_child_result_fingerprint", "")) {
                return std::unexpected("PAM substack changed during an interrupted upgrade");
            }
            if (const auto restored = atomic_write(child, *backup_content, 0644); !restored) {
                return restored;
            }
            auto& mutable_record = (*journal)["targets"][id];
            mutable_record["state"] = "active";
            mutable_record.erase("upgrade_child_backup");
            mutable_record.erase("pending_child_result_fingerprint");
            if (const auto saved = save_journal(root, *journal); !saved) {
                return saved;
            }
            (void)remove_regular_file(backup);
            continue;
        }
        interrupted.push_back(*target);
    }
    for (const auto target : interrupted) {
        if (const auto rolled_back = rollback_pam_integration(root, target); !rolled_back) {
            return rolled_back;
        }
    }
    return {};
}

std::expected<void, std::string> rollback_all_pam_integrations(
    const std::filesystem::path& root) {
    if (const auto recovered = recover_interrupted_pam_transactions(root); !recovered) {
        return recovered;
    }
    auto journal = load_journal(root);
    if (!journal) {
        return std::unexpected(journal.error());
    }
    auto targets = std::vector<TargetKind>{};
    for (const auto& [id, _] : (*journal)["targets"].items()) {
        const auto target = target_from_id(id);
        if (!target) {
            return std::unexpected("deployment journal contains an unknown target");
        }
        targets.push_back(*target);
    }
    for (const auto target : targets) {
        if (const auto rolled_back = rollback_pam_integration(root, target); !rolled_back) {
            return rolled_back;
        }
    }
    return {};
}

std::expected<void, std::string> check_package_upgrade(
    const std::filesystem::path& root,
    std::string_view candidate_version) {
    const auto candidate = parse_version(candidate_version);
    if (!candidate) {
        return std::unexpected("candidate package version is invalid");
    }
    const auto journal = load_journal(root);
    if (!journal) {
        return std::unexpected(journal.error());
    }
    if ((*journal)["targets"].empty()) {
        return {};
    }
    const auto recorded_text = journal->value("package_version", "0.0.0");
    const auto recorded = parse_version(recorded_text);
    if (!recorded) {
        return std::unexpected("deployment journal package version is invalid");
    }
    if (*candidate < *recorded) {
        return std::unexpected(std::format(
            "refusing package downgrade from managed version {} to {}",
            recorded_text, candidate_version));
    }
    return {};
}

std::string snapshot_json(const DeploymentSnapshot& snapshot) {
    auto targets = json::array();
    for (const auto& target : snapshot.targets) {
        targets.push_back({
            {"id", target_id(target.kind)},
            {"service", target.service},
            {"role", target.role == TargetRole::kLogin ? "login"
                : target.role == TargetRole::kLock ? "lock" : "login-and-lock"},
            {"state", target_state_id(target.state)},
            {"path", target.effective_path.string()},
            {"password_fallback", target.password_fallback},
            {"wallet_modules", target.wallet_modules},
            {"wallet_token_enabled", target.wallet_token_enabled},
            {"detail", target.detail},
        });
    }
    return json{{"version", 1}, {"targets", std::move(targets)}}.dump();
}

std::string plan_json(const PamPlan& plan) {
    return json{
        {"version", 1},
        {"target", target_id(plan.target)},
        {"service", plan.service},
        {"source", plan.source_path.string()},
        {"destination", plan.destination_path.string()},
        {"substack", plan.child_path.string()},
        {"wallet_token", plan.wallet_token_requested},
        {"already_managed", plan.already_managed},
        {"password_fallback", true},
    }.dump();
}

} // namespace su::deploy
