#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <papilio/format.hpp>

namespace su::app::i18n {

struct LocaleDescriptor {
    std::string id;
    std::string display_name;
    std::string file;
};

struct I18nConfig {
    std::string default_locale;
    std::vector<LocaleDescriptor> locales;
};

struct I18nCatalog {
    std::string locale;
    std::string display_name;
    std::unordered_map<std::string, std::string> messages;
};

struct I18nStore {
    I18nConfig config;
    std::unordered_map<std::string, I18nCatalog> catalogs;
    std::string diagnostics;
};

const I18nStore& app_i18n();

std::string default_locale(const I18nStore& store);
std::string cycle_locale(const I18nStore& store, std::string_view current_locale);
std::string locale_display_name(const I18nStore& store, std::string_view locale);
std::string text(const I18nStore& store, std::string_view locale, std::string_view key);
std::string diagnostics_text(const I18nStore& store);

template <typename... Args>
std::string format_pattern(std::string_view pattern, Args&&... args) {
    using context_type = papilio::basic_format_context<std::back_insert_iterator<std::string>, char>;
    auto formatted = std::string{};
    papilio::vformat_to(
        std::back_inserter(formatted),
        pattern,
        papilio::make_format_args<context_type>(std::forward<Args>(args)...));
    return formatted;
}

template <typename... Args>
std::string format(
    const I18nStore& store,
    std::string_view locale,
    std::string_view key,
    Args&&... args
) {
    return format_pattern(
        text(store, locale, key),
        std::forward<Args>(args)...);
}

}  // namespace su::app::i18n
