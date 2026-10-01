module;
#include <nlohmann/json.hpp>
#include "about_data.h"

export module su.app.about;
import std;

export namespace su::app {
struct Contributor {
    std::string name;
    std::string role;
    std::string url;
};
struct OpenSourceComponent {
    std::string name;
    std::string license;
};
struct LicenseDocument {
    std::string name;
    std::string text;
};
struct AboutInfo {
    std::vector<Contributor> contributors;
    std::vector<OpenSourceComponent> components;
    std::vector<LicenseDocument> documents;
};

AboutInfo about_info() {
    const auto data = nlohmann::json::parse(su_about_json);
    auto info = AboutInfo{};
    for (const auto& entry : data.at("contributors")) {
        info.contributors.push_back({entry.at("name"), entry.at("role"), entry.at("url")});
    }
    for (const auto& entry : data.at("components")) {
        info.components.push_back({entry.at("name"), entry.at("license")});
    }
    for (const auto& entry : data.at("documents")) {
        info.documents.push_back({entry.at("name"), entry.at("text")});
    }
    return info;
}
}  // namespace su::app
