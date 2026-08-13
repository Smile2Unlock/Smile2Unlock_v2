// MinGW workaround: Arch mingw-w64-gcc 16.1.0 ships libstdc++ without the
// terminal path used by std::print (bits/ostream_print.h and bits/print.h
// declare std::__open_terminal/std::__write_to_terminal but the library lacks
// the symbols). Returning nullptr from __open_terminal makes std::print fall
// back to the regular streambuf/FILE path, which is fully functional.

#include <cstdio>
#include <span>
#include <streambuf>
#include <system_error>

namespace std {

void* __open_terminal(streambuf*) {
    return nullptr;
}

void* __open_terminal(FILE*) {
    return nullptr;
}

error_code __write_to_terminal(void*, span<char>) {
    return {};
}

} // namespace std
