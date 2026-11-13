#include "platform/linux/deploy/account.h"

#include "platform/linux/deploy/safe_read.h"

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

namespace su::deploy {
namespace {

constexpr std::size_t kMaxAccountFileBytes = 4U * 1024U * 1024U;
constexpr std::size_t kPasswdNameField = 0;
constexpr std::size_t kPasswdHashField = 1;
constexpr std::size_t kPasswdUidField = 2;
constexpr std::size_t kShadowHashField = 1;
constexpr std::string_view kShadowPlaceholder = "x";

std::vector<std::string_view> split_fields(std::string_view line) {
    return line | std::views::split(':')
        | std::views::transform([](auto part) { return std::string_view(part); })
        | std::ranges::to<std::vector<std::string_view>>();
}

std::expected<std::string, std::string> read_account_file(const std::filesystem::path& path) {
    const auto opened = open_regular_nofollow(path);
    if (!opened) {
        return std::unexpected(std::format("cannot open {} to check the account password",
                                           path.string()));
    }
    return read_capped(*opened, kMaxAccountFileBytes).transform_error([&](ReadFailure) {
        return std::format("cannot read {} to check the account password", path.string());
    });
}

std::optional<std::vector<std::string_view>> find_entry(
    std::string_view text, std::size_t key_field, std::string_view key) {
    for (const auto line : text | std::views::split('\n')) {
        auto fields = split_fields(std::string_view(line));
        if (fields.size() > key_field && fields[key_field] == key) {
            return fields;
        }
    }
    return std::nullopt;
}

} // namespace

std::expected<AccountPassword, std::string> account_password_state(
    const std::filesystem::path& root, uid_t uid) {
    const auto passwd = read_account_file(root / "etc" / "passwd");
    if (!passwd) {
        return std::unexpected(passwd.error());
    }
    const auto entry = find_entry(*passwd, kPasswdUidField, std::to_string(uid));
    if (!entry || entry->size() <= kPasswdHashField) {
        return AccountPassword::kNotLocal;
    }
    const auto& hash = (*entry)[kPasswdHashField];
    if (hash.empty()) {
        return AccountPassword::kEmpty;
    }
    if (hash != kShadowPlaceholder) {
        return AccountPassword::kSet;
    }
    const auto shadow = read_account_file(root / "etc" / "shadow");
    if (!shadow) {
        return std::unexpected(shadow.error());
    }
    const auto shadow_entry = find_entry(*shadow, kPasswdNameField, (*entry)[kPasswdNameField]);
    if (!shadow_entry || shadow_entry->size() <= kShadowHashField) {
        return std::unexpected(std::format("no shadow entry for account {}",
                                           std::string((*entry)[kPasswdNameField])));
    }
    return (*shadow_entry)[kShadowHashField].empty() ? AccountPassword::kEmpty
                                                    : AccountPassword::kSet;
}

} // namespace su::deploy
