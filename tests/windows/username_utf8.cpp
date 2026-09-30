#include "username_utf8.h"
#include "app/username_initial.h"

#if SU_TEST_HAS_SLINT
#include <slint.h>
#endif

#include <array>
#include <iostream>
#include <string>

extern "C" int su_win_username_for_uid(char* out, unsigned long cap);

int main() {
    auto failures = 0;
    const auto check = [&failures](bool ok, const char* message) {
        if (!ok) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };
    char output[1025] = {};
    const auto conversion = [&](std::wstring_view wide, std::string_view expected) {
        const auto status = su::windows::copy_username_utf8(wide, output, sizeof(output));
        check(status == 0 && output == expected, "Windows username converted to exact UTF-8 bytes");
#if SU_TEST_HAS_SLINT
        const auto name = slint::SharedString(output);
        const auto initial = slint::SharedString(su::app::username_initial(output));
        check(std::string_view(name) == expected, "complete username survives Slint FFI");
        check(std::string_view(initial) == su::app::username_initial(expected),
              "avatar initial survives Slint FFI");
#endif
    };
    conversion(L"alice", "alice");
    conversion(L"\u4e8e\u5149\u8fdc", "\u4e8e\u5149\u8fdc");
    conversion(L"\u00e9mile", "\u00e9mile");
    conversion(L"\U0001f600user", "\U0001f600user");
    auto long_name = std::wstring(256, L'\u4e8e');
    auto long_utf8 = std::string{};
    for (auto i = 0; i < 256; ++i) {
        long_utf8 += "\u4e8e";
    }
    conversion(long_name, long_utf8);

    std::array<char, 8> bounded{};
    bounded.fill('x');
    check(su::windows::copy_username_utf8(L"\u4e8e", bounded.data(), 3) == -1,
          "UTF-8 plus terminator must fit output capacity");
    check(bounded[0] == '\0' && bounded[3] == 'x', "failed conversion clears output and preserves bounds");
    check(su::windows::copy_username_utf8(L"\u4e8e", bounded.data(), 4) == 0
          && std::string_view(bounded.data()) == "\u4e8e" && bounded[4] == 'x',
          "exact UTF-8 buffer capacity works");
    const auto surrogate = std::wstring(1, static_cast<wchar_t>(0xD800));
    check(su::windows::copy_username_utf8(surrogate, output, sizeof(output)) == -1
          && output[0] == '\0', "unpaired UTF-16 surrogate is rejected");
    check(su::windows::copy_username_utf8(L"alice", nullptr, 0) == -1, "null buffer is rejected");
    check(su_win_username_for_uid(nullptr, 0) == -1, "username lookup rejects null buffer");
    check(su_win_username_for_uid(output, 1) == -1 && output[0] == '\0',
          "username lookup rejects undersized buffer");
    check(su_win_username_for_uid(output, sizeof(output)) == 0 && output[0] != '\0',
          "actual Windows username lookup succeeds");
    check(::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, output, -1, nullptr, 0) > 0,
          "actual Windows username is valid UTF-8");
#if SU_TEST_HAS_SLINT
    const auto actual_name = slint::SharedString(output);
    check(!actual_name.empty(), "actual Windows username survives Slint FFI");
#endif
    if (failures == 0) {
        std::cout << "PASS: Windows username UTF-8, Unicode fixtures, and buffer bounds\n";
    }
    return failures == 0 ? 0 : 1;
}
