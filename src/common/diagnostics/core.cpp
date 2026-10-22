#include "core.h"
#include <algorithm>
#include <cctype>

namespace su::diagnostics {
std::vector<su::log::Field> to_fields(const Error& error) {
    return {su::log::field("diagnostics_error", static_cast<int>(error.code)),
            su::log::field("cause", error.detail)};
}
std::string archive_name(std::string_view timestamp, bool windows, std::size_t collision) {
    return "smile2unlock-diagnostics-" + std::string(timestamp)
        + (collision ? "-" + std::to_string(collision) : "") + (windows ? ".zip" : ".tar.gz");
}
std::string available_archive_name(std::string_view timestamp, bool windows,
                                 const std::vector<std::string>& existing) {
    for (std::size_t index = 0;; ++index) {
        const auto name = archive_name(timestamp, windows, index);
        if (std::ranges::find(existing, name) == existing.end()) return name;
    }
}
bool log_file_name(std::string_view name, bool app_only) {
    if (name.find_first_of("/\\:") != name.npos) return false;
    const auto pos = name.rfind(".log");
    if (pos == name.npos || pos == 0 || (app_only && name.substr(0, pos) != "app")) return false;
    const auto suffix = name.substr(pos + 4);
    return suffix.empty() || (suffix.size() > 1 && suffix.front() == '.'
        && std::ranges::all_of(suffix.substr(1), [](char c) { return c >= '0' && c <= '9'; }));
}
bool staging_child(std::string_view name) {
    return name.size() == 16 && std::ranges::all_of(name, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool valid_helper_path(std::string_view root, std::string_view candidate) {
    const auto normalize = [](std::string_view value) {
        auto result = std::string(value);
        for (auto& c : result) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return result;
    };
    auto base = normalize(root);
    const auto path = normalize(candidate);
    if (base.empty() || base.find("/../") != base.npos || base.find("/./") != base.npos) return false;
    if (base.back() != '/') base += '/';
    return path.starts_with(base) && staging_child(path.substr(base.size()));
}
std::string truncate_output(std::string_view text, std::size_t cap) {
    constexpr auto marker = std::string_view{"\n[output truncated]\n"};
    if (text.size() <= cap) return std::string(text);
    if (cap < marker.size()) return std::string(marker.substr(0, cap));
    auto end = cap - marker.size();
    while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return std::string(text.substr(0, end)) + std::string(marker);
}
std::string format_system(const std::vector<Section>& sections) {
    auto result = std::string{};
    for (const auto& section : sections) {
        result += "[" + section.name + "]\n";
        for (const auto& record : section.records) result += record.name + ": " + record.value + "\n";
        result += '\n';
    }
    return truncate_output(result);
}
std::string format_summary(const Summary& in) {
    auto sections = std::vector<Section>{
        {"Smile2Unlock diagnostics", {{"Product version", in.product_version}, {"Rust core version", in.core_version},
            {"OS", in.os}, {"Local time", in.local_time}}},
        {"Runtime", {{"Service available", in.service_available ? "yes" : "no"},
            {"Service reason", in.service_reason}, {"Storage protection", in.storage_protection},
            {"Recognizer available", in.recognizer_available ? "yes" : "no"},
            {"Camera count", std::to_string(in.cameras.size())}, {"Profile count", std::to_string(in.profile_count)},
            {"Models present", in.models_present ? "yes" : "no"}, {"Models directory", in.model_dir}}}};
    for (const auto& camera : in.cameras) sections[1].records.push_back({"Camera", camera});
    sections[1].records.insert(sections[1].records.end(), in.runtime.begin(), in.runtime.end());
    sections.insert(sections.end(), in.targets.begin(), in.targets.end());
    return format_system(sections);
}
} // namespace su::diagnostics
