#ifndef SU_COMMON_UTF8_PATH_H_
#define SU_COMMON_UTF8_PATH_H_

#include <filesystem>
#include <string>
#include <string_view>

namespace su {

// The Rust and UI boundaries carry UTF-8, independent of the native path type.
inline std::string path_utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

inline std::filesystem::path path_from_utf8(std::string_view bytes) {
    return std::filesystem::path(std::u8string_view{
        reinterpret_cast<const char8_t*>(bytes.data()), bytes.size()});
}

}  // namespace su

#endif
