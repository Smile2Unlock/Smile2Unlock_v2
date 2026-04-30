module;

#include <papilio/format.hpp>
#include <rfl/json.hpp>

export module su.app.i18n;

import std;
import su.app.runtime;

export namespace su::app::i18n {

/**
 * @brief 语言环境描述符
 */
struct LocaleDescriptor {
    std::string id;           ///< 语言环境ID（如 "zh-CN"）
    std::string display_name; ///< 显示名称（如 "简体中文"）
    std::string file;         ///< 对应的JSON文件名
};

/**
 * @brief 国际化配置
 */
struct I18nConfig {
    std::string default_locale;                    ///< 默认语言环境
    std::vector<LocaleDescriptor> locales;         ///< 支持的语言环境列表
};

/**
 * @brief 国际化目录
 */
struct I18nCatalog {
    std::string locale;                                        ///< 语言环境ID
    std::string display_name;                                  ///< 显示名称
    std::unordered_map<std::string, std::string> messages;     ///< 翻译消息映射
};

/**
 * @brief 国际化存储
 */
struct I18nStore {
    I18nConfig config;                                         ///< 配置信息
    std::unordered_map<std::string, I18nCatalog> catalogs;     ///< 所有语言目录
    std::string diagnostics;                                   ///< 诊断信息
};

/**
 * @brief 获取全局国际化存储单例
 * @return const I18nStore& 国际化存储引用
 */
const I18nStore& app_i18n();

/**
 * @brief 获取默认语言环境
 * @param store 国际化存储
 * @return std::string 默认语言环境ID
 */
std::string default_locale(const I18nStore& store);

/**
 * @brief 循环切换语言环境
 * 
 * 在配置的语言环境列表中循环切换，到达末尾后回到第一个。
 * 
 * @param store 国际化存储
 * @param current_locale 当前语言环境
 * @return std::string 下一个语言环境ID
 */
std::string cycle_locale(const I18nStore& store, std::string_view current_locale);

/**
 * @brief 获取语言环境的显示名称
 * 
 * @param store 国际化存储
 * @param locale 语言环境ID
 * @return std::string 显示名称
 */
std::string locale_display_name(const I18nStore& store, std::string_view locale);

/**
 * @brief 获取翻译文本
 * 
 * 查找指定语言环境的翻译，如果找不到则回退到默认语言环境。
 * 
 * @param store 国际化存储
 * @param locale 语言环境ID
 * @param key 翻译键
 * @return std::string 翻译文本，未找到则返回键本身
 */
std::string text(const I18nStore& store, std::string_view locale, std::string_view key);

/**
 * @brief 获取诊断信息文本
 * 
 * @param store 国际化存储
 * @return std::string 诊断信息
 */
std::string diagnostics_text(const I18nStore& store);

/**
 * @brief 格式化模式字符串
 * 
 * 使用 papilio 库进行字符串格式化，支持位置参数。
 * 
 * @param pattern 格式模式
 * @param args 格式化参数
 * @return std::string 格式化后的字符串
 */
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

/**
 * @brief 获取翻译并格式化
 * 
 * 先获取翻译文本，然后使用提供的参数进行格式化。
 * 
 * @param store 国际化存储
 * @param locale 语言环境ID
 * @param key 翻译键
 * @param args 格式化参数
 * @return std::string 格式化后的翻译文本
 */
template <typename... Args>
std::string format(const I18nStore& store, std::string_view locale, std::string_view key, Args&&... args) {
    return format_pattern(text(store, locale, key), std::forward<Args>(args)...);
}

}  // namespace su::app::i18n

namespace su::app::i18n {
namespace {

using std::filesystem::path;

/**
 * @brief 获取源代码根目录
 * 
 * 通过当前文件路径向上推导得到源代码根目录。
 * 
 * @return path 源代码根目录路径
 */
path source_root() {
    return path(__FILE__).parent_path().parent_path().parent_path().parent_path();
}

/**
 * @brief 获取所有可能的资源根目录候选列表
 * 
 * 包括当前工作目录、可执行文件目录及其父目录、源代码根目录。
 * 
 * @return std::vector<path> 去重排序后的候选根目录列表
 */
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

/**
 * @brief 查找国际化资源目录
 * 
 * 在候选根目录中搜索包含 assets/i18n/config.json 的目录。
 * 
 * @return std::optional<path> 国际化目录路径，未找到则返回 nullopt
 */
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

/**
 * @brief 获取内置配置
 * 
 * 返回硬编码的默认国际化配置，包含中文和英文。
 * 
 * @return I18nConfig 内置配置
 */
I18nConfig builtin_config() {
    return I18nConfig{
        .default_locale = "zh-CN",
        .locales = {
            LocaleDescriptor{.id = "zh-CN", .display_name = "简体中文", .file = "zh-CN.json"},
            LocaleDescriptor{.id = "en-US", .display_name = "English", .file = "en-US.json"},
        },
    };
}

/**
 * @brief 创建空存储
 * 
 * 返回使用内置配置但无翻译数据的空存储。
 * 
 * @return I18nStore 空存储
 */
I18nStore empty_store() {
    return I18nStore{
        .config = builtin_config(),
        .catalogs = {},
        .diagnostics = "assets/i18n/config.json not found; no translations loaded",
    };
}

/**
 * @brief 安全加载值或返回回退值
 * 
 * 尝试从 rfl::Result 中获取值，失败则记录错误并返回回退值。
 * 
 * @param fallback 回退值
 * @param result 加载结果
 * @param context 上下文描述
 * @param errors 错误收集列表
 * @return T 加载的值或回退值
 */
template <typename T>
T value_or(T fallback, rfl::Result<T> result, std::string_view context, std::vector<std::string>& errors) {
    if (result) {
        return std::move(result).value();
    }
    errors.push_back(format_pattern("{}: {}", context, result.error().what()));
    return fallback;
}

/**
 * @brief 从磁盘加载国际化存储
 * 
 * 加载流程：
 * 1. 查找国际化目录
 * 2. 加载 config.json 配置
 * 3. 加载各语言的翻译文件
 * 4. 验证并修复默认语言环境
 * 5. 收集诊断信息
 * 
 * @return I18nStore 加载的国际化存储
 */
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

/**
 * @brief 查找语言目录
 * 
 * 按优先级查找：指定语言 -> 默认语言 -> 第一个可用语言。
 * 
 * @param store 国际化存储
 * @param locale 目标语言环境
 * @return const I18nCatalog* 语言目录指针，未找到则返回 nullptr
 */
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

/**
 * @brief 查找翻译消息
 * 
 * @param catalog 语言目录
 * @param key 翻译键
 * @return std::string 翻译值，未找到则返回键本身
 */
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

/**
 * @brief 获取全局国际化存储单例
 */
const I18nStore& app_i18n() {
    static const I18nStore store = load_store_from_disk();
    return store;
}

/**
 * @brief 获取默认语言环境
 */
std::string default_locale(const I18nStore& store) {
    if (!store.config.default_locale.empty()) {
        return store.config.default_locale;
    }
    if (!store.catalogs.empty()) {
        return store.catalogs.begin()->first;
    }
    return "zh-CN";
}

/**
 * @brief 循环切换语言环境
 */
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

/**
 * @brief 获取语言环境的显示名称
 */
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

/**
 * @brief 获取翻译文本
 */
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

/**
 * @brief 获取诊断信息文本
 */
std::string diagnostics_text(const I18nStore& store) {
    return store.diagnostics.empty() ? "ready" : store.diagnostics;
}

}  // namespace su::app::i18n
