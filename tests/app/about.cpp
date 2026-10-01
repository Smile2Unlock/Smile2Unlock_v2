#include "app/web_link.h"

import std;
import su.app.about;

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
}

int main() {
    try {
        // Parsing the embedded JSON also rejects malformed UTF-8 strings.
        const auto info = su::app::about_info();
        require(!info.contributors.empty() && !info.components.empty(), "credits are missing");
        const auto document = [&info](std::string_view name) -> const std::string& {
            const auto found = std::ranges::find(info.documents, name, &su::app::LicenseDocument::name);
            require(found != info.documents.end(), std::format("missing license: {}", name));
            require(!found->text.empty(), "empty license document");
            return found->text;
        };
        require(document("Smile2Unlock / MIT").contains("Copyright (c) 2025 Smile2Unlock contributors"),
            "project copyright is missing");
        require(document("Smile2Unlock / MIT").contains("OTHER DEALINGS IN THE\nSOFTWARE."),
            "project license is truncated");
        require(document("Third-party notices").contains("SeetaFace"), "third-party notices are missing");
        document("slint/GPL-3.0-only.txt");
        document("slint/LicenseRef-Slint-Royalty-free-2.0.md");
        document("slint/LicenseRef-Slint-Software-3.0.md");
        document("rust-crates.md");
        document("rust/Unicode-3.0.txt");
        require(document("CImg-CeCILL-C.txt").contains("à"), "Latin-1 license must be converted to UTF-8");
        require(document("CImg-CeCILL-V2.txt").contains("à"), "license accents are missing");
        for (const auto& contributor : info.contributors) {
            require(!contributor.name.empty() && contributor.url.starts_with("https://"),
                "contributor credit or profile link is missing");
        }
        require(!su::app::open_web_link("file:///tmp/test"), "non-web scheme was accepted");
        require(!su::app::open_web_link("https://example.com/\ncommand"), "control character was accepted");
        require(!su::app::open_web_link(std::string_view{"https://example.com/\0suffix", 27}),
            "embedded null was accepted");
        std::cout << "About credits and " << info.documents.size()
                  << " offline license documents passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "About test failed: " << error.what() << '\n';
        return 1;
    }
}
