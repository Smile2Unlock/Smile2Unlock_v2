#include "noctalia.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>

namespace su::deploy {
namespace {

constexpr std::string_view kBrand = "noctalia";
constexpr std::string_view kPasswordStack = "login";
constexpr std::size_t kMaxServiceNameLength = 64;
constexpr std::size_t kThinAuthFieldCount = 3; // auth <include|substack> <stack>

// Privilege and system stacks that must never be rewritten on a lock screen's
// behalf, even if some package ever shipped a file that looked acceptable.
// Services owned by other deployment targets arrive through ServicePolicy so
// the list cannot drift from the target table.
constexpr std::array kSystemDenied{
    std::string_view{"su"},          std::string_view{"su-l"},
    std::string_view{"sudo"},        std::string_view{"sudo-i"},
    std::string_view{"polkit-1"},    std::string_view{"sshd"},
    std::string_view{"passwd"},      std::string_view{"chsh"},
    std::string_view{"chfn"},        std::string_view{"runuser"},
    std::string_view{"runuser-l"},   std::string_view{"systemd-user"},
    std::string_view{"system-auth"}, std::string_view{"system-login"},
    std::string_view{"system-local-login"}, std::string_view{"common-auth"},
    std::string_view{"login"},       std::string_view{"other"},
};

bool iequals(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

std::vector<std::string_view> fields_of(std::string_view line) {
    return line | std::views::split(' ')
        | std::views::transform([](auto part) { return std::string_view(part); })
        | std::views::filter([](std::string_view part) { return !part.empty(); })
        | std::ranges::to<std::vector<std::string_view>>();
}

// Comments run to the end of the line; tabs are ordinary separators in PAM.
std::string normalized_line(std::string_view raw) {
    auto line = std::string(raw.substr(0, raw.find('#')));
    std::ranges::replace_if(line, [](unsigned char c) { return std::isspace(c) != 0; }, ' ');
    return line;
}

bool is_stack_reference(std::string_view control) {
    return iequals(control, "include") || iequals(control, "substack");
}

} // namespace

bool is_valid_pam_service_name(std::string_view name) {
    return !name.empty() && name.size() <= kMaxServiceNameLength && name.front() != '.'
        && std::ranges::all_of(name, [](unsigned char c) {
               return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
           });
}

std::string_view service_rejection_text(ServiceRejection reason) {
    switch (reason) {
    case ServiceRejection::kInvalidName:
        return "the PAM service name is not a plain file name";
    case ServiceRejection::kDenied:
        return "the PAM service belongs to another component and must not be modified";
    case ServiceRejection::kNotNoctaliaNamed:
        return "the PAM service name does not identify Noctalia";
    case ServiceRejection::kChildService:
        return "the PAM service is Smile2Unlock's own managed substack";
    case ServiceRejection::kNotThinWrapper:
        return "the PAM service is not a thin wrapper around the login stack";
    }
    return "the PAM service is not acceptable";
}

std::expected<void, ServiceRejection> check_noctalia_service_name(
    std::string_view name, const ServicePolicy& policy) {
    if (!is_valid_pam_service_name(name)) {
        return std::unexpected(ServiceRejection::kInvalidName);
    }
    if (std::ranges::contains(kSystemDenied, name) || std::ranges::contains(policy.denied, name)) {
        return std::unexpected(ServiceRejection::kDenied);
    }
    if (!name.contains(kBrand)) {
        return std::unexpected(ServiceRejection::kNotNoctaliaNamed);
    }
    if (name == policy.child_service) {
        return std::unexpected(ServiceRejection::kChildService);
    }
    return {};
}

std::expected<void, ServiceRejection> check_noctalia_service(
    std::string_view name, std::string_view pam_content, const ServicePolicy& policy) {
    return check_noctalia_service_name(name, policy).and_then([&]() -> std::expected<void, ServiceRejection> {
        // Exactly one auth-type line, and it must delegate to `login` (pristine)
        // or to our own substack (already managed). `@include` is rejected
        // because it could smuggle in more auth rules unseen.
        auto auth_lines = 0;
        auto thin = false;
        for (const auto raw : pam_content | std::views::split('\n')) {
            const auto line = normalized_line(std::string_view(raw));
            const auto fields = fields_of(line);
            if (fields.empty()) {
                continue;
            }
            if (fields.front().starts_with('@')) {
                return std::unexpected(ServiceRejection::kNotThinWrapper);
            }
            if (!iequals(fields.front().substr(fields.front().starts_with('-') ? 1 : 0), "auth")) {
                continue;
            }
            ++auth_lines;
            thin = fields.size() == kThinAuthFieldCount && iequals(fields[0], "auth")
                && is_stack_reference(fields[1])
                && (fields[2] == kPasswordStack || fields[2] == policy.child_service);
        }
        return auth_lines == 1 && thin ? std::expected<void, ServiceRejection>{}
                                       : std::unexpected(ServiceRejection::kNotThinWrapper);
    });
}

} // namespace su::deploy
