module;

#include <nlohmann/json.hpp>

export module su.app.i18n;

import std;

export namespace su::app {

class LanguageCatalog {
public:
    static std::expected<LanguageCatalog, std::string> load(
        const std::filesystem::path& directory);

    [[nodiscard]] std::size_t size() const { return packs_.size(); }
    [[nodiscard]] std::vector<std::string> language_names() const;
    [[nodiscard]] std::string_view language_code(std::size_t index) const;
    [[nodiscard]] std::size_t select_language(
        std::optional<std::string_view> preferred_code,
        std::string_view system_locale) const;
    [[nodiscard]] std::string translate(std::size_t index, std::string_view key) const;
    [[nodiscard]] std::string translate_value(
        std::size_t index,
        std::string_view key,
        std::string_view value) const;

private:
    struct LanguagePack {
        std::string code;
        std::string name;
        std::unordered_map<std::string, std::string> strings;
    };

    static std::expected<LanguagePack, std::string> load_pack(
        const std::filesystem::path& path);

    std::vector<LanguagePack> packs_;
    std::size_t fallback_index_ = 0;
};

}  // namespace su::app

namespace su::app {

namespace {

std::string normalize_language_code(std::string_view code) {
    auto normalized = std::string(code);
    if (const auto suffix = normalized.find_first_of(".@"); suffix != std::string::npos) {
        normalized.resize(suffix);
    }
    std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
        return character == '_' ? '-' : static_cast<char>(std::tolower(character));
    });
    return normalized;
}

std::string replace_value(std::string text, std::string_view value) {
    constexpr auto placeholder = std::string_view{"{value}"};
    auto offset = std::size_t{0};
    while ((offset = text.find(placeholder, offset)) != std::string::npos) {
        text.replace(offset, placeholder.size(), value);
        offset += value.size();
    }
    return text;
}

}  // namespace

std::expected<LanguageCatalog::LanguagePack, std::string> LanguageCatalog::load_pack(
    const std::filesystem::path& path) {
    try {
        auto stream = std::ifstream(path);
        if (!stream) {
            return std::unexpected(std::format("cannot open {}", path.string()));
        }

        const auto document = nlohmann::json::parse(stream);
        if (!document.contains("code") || !document["code"].is_string()
            || !document.contains("name") || !document["name"].is_string()
            || !document.contains("strings") || !document["strings"].is_object()) {
            return std::unexpected(std::format("invalid language pack schema: {}", path.string()));
        }

        LanguageCatalog::LanguagePack pack{
            .code = document["code"].get<std::string>(),
            .name = document["name"].get<std::string>(),
            .strings = {},
        };
        if (pack.code.empty() || pack.name.empty()) {
            return std::unexpected(std::format("language code and name must not be empty: {}", path.string()));
        }

        for (const auto& [key, value] : document["strings"].items()) {
            if (!value.is_string()) {
                return std::unexpected(std::format("translation '{}' is not a string: {}", key, path.string()));
            }
            pack.strings.emplace(key, value.get<std::string>());
        }
        return pack;
    } catch (const std::exception& error) {
        return std::unexpected(std::format("failed to parse {}: {}", path.string(), error.what()));
    }
}

std::expected<LanguageCatalog, std::string> LanguageCatalog::load(
    const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return std::unexpected(std::format("language directory is unavailable: {}", directory.string()));
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            return std::unexpected(std::format("failed to scan language directory: {}", error.message()));
        }
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);

    LanguageCatalog catalog;
    for (const auto& file : files) {
        auto pack = load_pack(file);
        if (!pack) {
            std::println(stderr, "[i18n] {}", pack.error());
            continue;
        }
        catalog.packs_.push_back(std::move(*pack));
    }
    if (catalog.packs_.empty()) {
        return std::unexpected(std::format("no valid language packs found in {}", directory.string()));
    }

    auto has_english_fallback = false;
    for (auto index = std::size_t{0}; index < catalog.packs_.size(); ++index) {
        if (normalize_language_code(catalog.packs_[index].code) == "en") {
            catalog.fallback_index_ = index;
            has_english_fallback = true;
            break;
        }
    }
    if (!has_english_fallback) {
        return std::unexpected(std::format(
            "English fallback language pack is missing from {}", directory.string()));
    }
    return catalog;
}

std::vector<std::string> LanguageCatalog::language_names() const {
    std::vector<std::string> names;
    names.reserve(packs_.size());
    for (const auto& pack : packs_) {
        names.push_back(pack.name);
    }
    return names;
}

std::string_view LanguageCatalog::language_code(std::size_t index) const {
    if (index >= packs_.size()) {
        index = fallback_index_;
    }
    return packs_[index].code;
}

std::size_t LanguageCatalog::select_language(
    std::optional<std::string_view> preferred_code,
    std::string_view system_locale) const {
    const auto find_code = [this](std::string_view requested) -> std::optional<std::size_t> {
        const auto normalized = normalize_language_code(requested);
        for (auto index = std::size_t{0}; index < packs_.size(); ++index) {
            if (normalize_language_code(packs_[index].code) == normalized) {
                return index;
            }
        }
        const auto primary = normalized.substr(0, normalized.find('-'));
        for (auto index = std::size_t{0}; index < packs_.size(); ++index) {
            const auto candidate = normalize_language_code(packs_[index].code);
            if (candidate.substr(0, candidate.find('-')) == primary) {
                return index;
            }
        }
        return std::nullopt;
    };

    if (preferred_code) {
        if (const auto selected = find_code(*preferred_code)) {
            return *selected;
        }
    }
    if (const auto selected = find_code(system_locale)) {
        return *selected;
    }
    return fallback_index_;
}

std::string LanguageCatalog::translate(std::size_t index, std::string_view key) const {
    if (index >= packs_.size()) {
        index = fallback_index_;
    }
    const auto find = [key](const LanguagePack& pack) -> const std::string* {
        const auto entry = pack.strings.find(std::string(key));
        return entry == pack.strings.end() ? nullptr : &entry->second;
    };
    if (const auto* text = find(packs_[index])) {
        return *text;
    }
    if (const auto* text = find(packs_[fallback_index_])) {
        return *text;
    }
    return std::string(key);
}

std::string LanguageCatalog::translate_value(
    std::size_t index,
    std::string_view key,
    std::string_view value) const {
    return replace_value(translate(index, key), value);
}

}  // namespace su::app
