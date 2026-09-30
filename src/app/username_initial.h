#ifndef SU_APP_USERNAME_INITIAL_H_
#define SU_APP_USERNAME_INITIAL_H_

#include <string>
#include <string_view>

namespace su::app {

// Slint requires valid UTF-8. Preserve the first complete Unicode scalar;
// byte-wise uppercasing would pass a truncated multibyte sequence to its FFI.
inline std::string username_initial(std::string_view username) {
    if (username.empty()) {
        return "U";
    }
    const auto lead = static_cast<unsigned char>(username.front());
    if (lead < 0x80) {
        const auto initial = lead >= 'a' && lead <= 'z' ? lead - 'a' + 'A' : lead;
        return std::string(1, static_cast<char>(initial));
    }
    const auto length = lead >= 0xC2 && lead <= 0xDF ? std::size_t{2}
        : lead >= 0xE0 && lead <= 0xEF ? std::size_t{3}
        : lead >= 0xF0 && lead <= 0xF4 ? std::size_t{4} : std::size_t{0};
    if (length == 0 || username.size() < length) {
        return "U";
    }
    for (auto index = std::size_t{1}; index < length; ++index) {
        const auto byte = static_cast<unsigned char>(username[index]);
        if (byte < 0x80 || byte > 0xBF) {
            return "U";
        }
    }
    const auto second = static_cast<unsigned char>(username[1]);
    // Exclude overlong encodings, UTF-16 surrogates, and values > U+10FFFF.
    if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second >= 0xA0)
        || (lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second > 0x8F)) {
        return "U";
    }
    return std::string(username.substr(0, length));
}

}  // namespace su::app

#endif  // SU_APP_USERNAME_INITIAL_H_
