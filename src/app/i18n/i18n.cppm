module;

#include <papilio/format.hpp>
#include <rfl/json.hpp>

export module su.app.i18n;

import std;
import su.app.runtime;

export namespace su::app::i18n {

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
std::string format(const I18nStore& store, std::string_view locale, std::string_view key, Args&&... args) {
    return format_pattern(text(store, locale, key), std::forward<Args>(args)...);
}

}  // namespace su::app::i18n

namespace su::app::i18n {
namespace {

using std::filesystem::path;

path source_root() {
    return path(__FILE__).parent_path().parent_path().parent_path().parent_path();
}

std::vector<path> candidate_roots() {
    auto roots = std::vector<path>{
        std::filesystem::current_path(),
        runtime::executable_dir(),
        runtime::executable_dir().parent_path(),
        source_root(),
    };

    std::ranges::sort(roots);
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

std::optional<path> find_i18n_dir() {
    const auto roots = candidate_roots();
    const auto match = std::ranges::find_if(roots, [](const path& root) {
        return std::filesystem::exists(root / "assets" / "i18n" / "config.json");
    });
    if (match == roots.end()) {
        return std::nullopt;
    }
    return *match / "assets" / "i18n";
}

I18nConfig builtin_config() {
    return I18nConfig{
        .default_locale = "zh-CN",
        .locales = {
            LocaleDescriptor{.id = "zh-CN", .display_name = "简体中文", .file = "zh-CN.json"},
            LocaleDescriptor{.id = "en-US", .display_name = "English", .file = "en-US.json"},
        },
    };
}

I18nStore empty_store() {
    return I18nStore{
        .config = builtin_config(),
        .catalogs = {},
        .diagnostics = "assets/i18n/config.json not found; no translations loaded",
    };
}

template <typename T>
T value_or(T fallback, rfl::Result<T> result, std::string_view context, std::vector<std::string>& errors) {
    if (result) {
        return std::move(result).value();
    }
    errors.push_back(format_pattern("{}: {}", context, result.error().what()));
    return fallback;
}

I18nStore load_store_from_disk() {
    auto errors = std::vector<std::string>{};
    const auto i18n_dir = find_i18n_dir();
    if (!i18n_dir) {
        auto store = empty_store();
        store.diagnostics = "assets/i18n/config.json not found";
        return store;
    }

    auto store = I18nStore{
        .config = value_or(builtin_config(), rfl::json::load<I18nConfig>((*i18n_dir / "config.json").string()), "config.json", errors),
        .catalogs = {},
        .diagnostics = {},
    };

    const auto descriptors = store.config.locales;
    const auto load_catalog = [&](const LocaleDescriptor& descriptor) {
        const auto catalog_path = *i18n_dir / descriptor.file;
        const auto fallback = I18nCatalog{
            .locale = descriptor.id,
            .display_name = descriptor.display_name,
            .messages = {},
        };

        auto catalog = value_or(fallback, rfl::json::load<I18nCatalog>(catalog_path.string()), descriptor.file, errors);
        if (catalog.locale.empty()) {
            catalog.locale = descriptor.id;
        }
        if (catalog.display_name.empty()) {
            catalog.display_name = descriptor.display_name;
        }
        return catalog;
    };

    std::ranges::for_each(descriptors, [&](const LocaleDescriptor& descriptor) {
        auto catalog = load_catalog(descriptor);
        store.catalogs.insert_or_assign(catalog.locale, std::move(catalog));
    });

    if (store.catalogs.empty()) {
        store = empty_store();
        errors.push_back("no catalogs loaded");
    }

    const auto fallback_locale = store.config.default_locale.empty() ? std::string("zh-CN") : store.config.default_locale;
    if (!store.catalogs.contains(fallback_locale)) {
        const auto first_catalog = std::ranges::find_if(store.catalogs, [](const auto&) { return true; });
        if (first_catalog != store.catalogs.end()) {
            store.config.default_locale = first_catalog->first;
        }
        errors.push_back(format_pattern("default locale '{}' was missing and has been repaired", fallback_locale));
    }

    store.diagnostics = errors.empty() ? "loaded from assets/i18n" : std::accumulate(
        std::next(errors.begin()),
        errors.end(),
        errors.front(),
        [](std::string joined, const std::string& item) {
            return joined + " | " + item;
        });
    return store;
}

const I18nCatalog* find_catalog(const I18nStore& store, std::string_view locale) {
    if (const auto it = store.catalogs.find(std::string(locale)); it != store.catalogs.end()) {
        return &it->second;
    }
    if (const auto fallback = store.catalogs.find(default_locale(store)); fallback != store.catalogs.end()) {
        return &fallback->second;
    }
    if (!store.catalogs.empty()) {
        return &store.catalogs.begin()->second;
    }
    return nullptr;
}

std::string find_message(const I18nCatalog* catalog, std::string_view key) {
    if (catalog == nullptr) {
        return std::string(key);
    }
    if (const auto it = catalog->messages.find(std::string(key)); it != catalog->messages.end()) {
        return it->second;
    }
    return std::string(key);
}

}  // namespace

const I18nStore& app_i18n() {
    static const I18nStore store = load_store_from_disk();
    return store;
}

std::string default_locale(const I18nStore& store) {
    if (!store.config.default_locale.empty()) {
        return store.config.default_locale;
    }
    if (!store.catalogs.empty()) {
        return store.catalogs.begin()->first;
    }
    return "zh-CN";
}

std::string cycle_locale(const I18nStore& store, std::string_view current_locale) {
    auto ordered = std::vector<std::string>{};
    std::ranges::transform(store.config.locales, std::back_inserter(ordered), [](const LocaleDescriptor& descriptor) {
        return descriptor.id;
    });
    if (ordered.empty()) {
        return default_locale(store);
    }

    const auto current = std::ranges::find(ordered, current_locale);
    if (current == ordered.end() || std::next(current) == ordered.end()) {
        return ordered.front();
    }
    return *std::next(current);
}

std::string locale_display_name(const I18nStore& store, std::string_view locale) {
    if (const auto catalog = find_catalog(store, locale)) {
        if (!catalog->display_name.empty()) {
            return catalog->display_name;
        }
    }

    const auto descriptor = std::ranges::find_if(store.config.locales, [&](const LocaleDescriptor& item) {
        return item.id == locale;
    });
    if (descriptor != store.config.locales.end() && !descriptor->display_name.empty()) {
        return descriptor->display_name;
    }
    return std::string(locale);
}

std::string text(const I18nStore& store, std::string_view locale, std::string_view key) {
    const auto requested = find_catalog(store, locale);
    const auto requested_value = find_message(requested, key);
    if (requested_value != key) {
        return requested_value;
    }

    const auto fallback = find_catalog(store, default_locale(store));
    const auto fallback_value = find_message(fallback, key);
    if (fallback_value != key) {
        return fallback_value;
    }
    return std::string(key);
}

std::string diagnostics_text(const I18nStore& store) {
    return store.diagnostics.empty() ? "ready" : store.diagnostics;
}

}  // namespace su::app::i18n
