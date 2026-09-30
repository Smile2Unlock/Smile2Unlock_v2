#include "app/username_initial.h"

#if SU_TEST_HAS_SLINT
#include <slint.h>
#endif

#include <iostream>
#include <utility>

int main() {
    const std::pair<std::string_view, std::string_view> valid[] = {
        {"", "U"}, {"alice", "A"}, {"Bob", "B"}, {"9user", "9"},
        {"\u4e8e\u5149\u8fdc", "\u4e8e"}, {"\u00e9mile", "\u00e9"},
        {"\U0001f600user", "\U0001f600"}, {"\U00020000user", "\U00020000"},
        {"\xC2\x80", "\xC2\x80"}, {"\xDF\xBF", "\xDF\xBF"},
        {"\xE0\xA0\x80", "\xE0\xA0\x80"}, {"\xED\x9F\xBF", "\xED\x9F\xBF"},
        {"\xEE\x80\x80", "\xEE\x80\x80"}, {"\xEF\xBF\xBF", "\xEF\xBF\xBF"},
        {"\xF0\x90\x80\x80", "\xF0\x90\x80\x80"},
        {"\xF4\x8F\xBF\xBF", "\xF4\x8F\xBF\xBF"},
    };
    const std::string_view invalid[] = {
        "\x80", "\xBF", "\xC0\xAF", "\xC1\xBF", "\xF5\x80\x80\x80", "\xFF",
        "\xC2", "\xE4\xBA", "\xF0\x9F\x98", "\xE4\x28\xAE", "\xC2\x7F",
        "\xE0\x80\x80", "\xED\xA0\x80", "\xED\xBF\xBF",
        "\xF0\x80\x80\x80", "\xF4\x90\x80\x80",
    };
    auto failures = 0;
    const auto check = [&failures](std::string_view input, std::string_view expected) {
        const auto result = su::app::username_initial(input);
        if (result != expected) {
            std::cerr << "FAIL: avatar initial for input with " << input.size() << " bytes\n";
            ++failures;
        }
#if SU_TEST_HAS_SLINT
        // This is the FFI call that aborted startup with a partial UTF-8 byte.
        const auto shared = slint::SharedString(result);
        if (std::string_view(shared) != expected) {
            std::cerr << "FAIL: avatar initial did not survive Slint conversion\n";
            ++failures;
        }
#endif
    };
    for (const auto& [input, expected] : valid) {
        check(input, expected);
#if SU_TEST_HAS_SLINT
        const auto shared_username = slint::SharedString(input);
        if (std::string_view(shared_username) != input) {
            ++failures;
        }
#endif
    }
    for (const auto input : invalid) {
        check(input, "U");
    }
    if (failures == 0) {
        std::cout << "PASS: UTF-8 avatar initials and malformed input fallback";
#if SU_TEST_HAS_SLINT
        std::cout << " through Slint FFI";
#endif
        std::cout << '\n';
    }
    return failures == 0 ? 0 : 1;
}
