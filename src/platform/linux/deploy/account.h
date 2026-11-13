#pragma once

#include <expected>
#include <filesystem>
#include <string>

#include <sys/types.h>

namespace su::deploy {

enum class AccountPassword {
    kSet,          // A hash, or a lock marker such as "!" / "*".
    kEmpty,        // The hash field is empty: `nullok` stacks accept any input.
    kNotLocal,     // Not in /etc/passwd (LDAP, SSSD, ...): nothing local to check.
};

// Looks the uid up in <root>/etc/passwd and, when needed, <root>/etc/shadow.
// An unreadable shadow file is an error rather than "fine": the caller is about
// to enable submitting an empty password, so it must be able to prove safety.
[[nodiscard]] std::expected<AccountPassword, std::string> account_password_state(
    const std::filesystem::path& root, uid_t uid);

} // namespace su::deploy
