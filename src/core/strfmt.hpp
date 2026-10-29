#pragma once
// piece::format — std::format, or a minimal stand-in on toolchains without it.
//
// The libretro buildbot builds Linux cores with gcc-9 (Ubuntu xenial image) to
// keep the glibc baseline low, and libstdc++ only gained <format> in gcc-13.
// Code compiled into the libretro core therefore calls piece::format instead
// of std::format.  When the standard library provides <format>, piece::format
// IS std::format; otherwise the fallback below implements the small subset of
// the replacement-field grammar this code base uses:
//
//   {}  {:d}  {:x}  {:X}  {:s}  with an optional '0' flag and width,
//   e.g. {:06X}, {:2d}.  "{{" and "}}" are literal braces.
//
// Arguments may be integers, bool, char, const char* or std::string(_view).
//
// Apple platforms always use the fallback: the buildbot targets macOS 10.15 /
// iOS 8, and Apple's libc++ marks parts of <format> (floating-point to_chars)
// unavailable below macOS 13.3, which can break std::format at compile time.
// Define PIECE_FORCE_STRFMT_FALLBACK to exercise the fallback elsewhere.

#include <string>
#include <string_view>
#if __has_include(<version>)
#include <version>
#endif

#if defined(__cpp_lib_format) && !defined(__APPLE__) && !defined(PIECE_FORCE_STRFMT_FALLBACK)

#include <format>

namespace piece {
using std::format;
} // namespace piece

#else

#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace piece {
namespace strfmt_detail {

struct Arg {
    enum class Kind { Signed, Unsigned, Str } kind;
    long long          s = 0;
    unsigned long long u = 0;
    std::string_view   str;
};

template <typename T>
Arg make_arg(const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return {Arg::Kind::Str, 0, 0, v ? "true" : "false"};
    } else if constexpr (std::is_same_v<T, char>) {
        return {Arg::Kind::Str, 0, 0, std::string_view(&v, 1)};
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        return {Arg::Kind::Signed, static_cast<long long>(v), 0, {}};
    } else if constexpr (std::is_integral_v<T>) {
        return {Arg::Kind::Unsigned, 0, static_cast<unsigned long long>(v), {}};
    } else {
        return {Arg::Kind::Str, 0, 0, std::string_view(v)};
    }
}

inline void append_field(std::string& out, const Arg& a, std::string_view spec) {
    bool zero = false;
    std::size_t width = 0;
    char type = 0;
    std::size_t p = 0;
    if (p < spec.size() && spec[p] == '0') { zero = true; ++p; }
    while (p < spec.size() && spec[p] >= '0' && spec[p] <= '9')
        width = width * 10 + static_cast<std::size_t>(spec[p++] - '0');
    if (p < spec.size()) type = spec[p++];
    if (p != spec.size())
        throw std::runtime_error("piece::format: unsupported format spec");

    std::string body;
    bool neg = false;
    if (a.kind == Arg::Kind::Str) {
        if (type != 0 && type != 's')
            throw std::runtime_error("piece::format: bad type for string");
        body.assign(a.str);
        if (body.size() < width)
            body.append(width - body.size(), ' ');   // strings left-align
        out += body;
        return;
    }

    unsigned long long mag = a.u;
    if (a.kind == Arg::Kind::Signed) {
        neg = a.s < 0;
        mag = neg ? 0ULL - static_cast<unsigned long long>(a.s)
                  : static_cast<unsigned long long>(a.s);
    }
    const char* digits;
    unsigned base;
    switch (type) {
    case 0: case 'd': digits = "0123456789";       base = 10; break;
    case 'x':         digits = "0123456789abcdef"; base = 16; break;
    case 'X':         digits = "0123456789ABCDEF"; base = 16; break;
    default:
        throw std::runtime_error("piece::format: bad type for integer");
    }
    do {
        body.insert(body.begin(), digits[mag % base]);
        mag /= base;
    } while (mag != 0);

    std::size_t len = body.size() + (neg ? 1 : 0);
    if (zero) {
        if (len < width) body.insert(0, width - len, '0');
        if (neg) body.insert(body.begin(), '-');
    } else {
        if (neg) body.insert(body.begin(), '-');
        if (len < width) body.insert(0, width - len, ' ');  // numbers right-align
    }
    out += body;
}

inline std::string vformat(std::string_view fmt, const Arg* args, std::size_t nargs) {
    std::string out;
    std::size_t next = 0;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        char c = fmt[i];
        if (c == '}') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '}') ++i;
            out += '}';
            continue;
        }
        if (c != '{') { out += c; continue; }
        if (i + 1 < fmt.size() && fmt[i + 1] == '{') { out += '{'; ++i; continue; }
        std::size_t close = fmt.find('}', i);
        if (close == std::string_view::npos)
            throw std::runtime_error("piece::format: unterminated field");
        std::string_view field = fmt.substr(i + 1, close - i - 1);
        if (!field.empty() && field[0] != ':')
            throw std::runtime_error("piece::format: positional args unsupported");
        if (next >= nargs)
            throw std::runtime_error("piece::format: too few arguments");
        append_field(out, args[next++], field.empty() ? field : field.substr(1));
        i = close;
    }
    return out;
}

} // namespace strfmt_detail

template <typename... Args>
std::string format(std::string_view fmt, const Args&... args) {
    const strfmt_detail::Arg list[sizeof...(Args) + 1] = {
        strfmt_detail::make_arg(args)..., {}};
    return strfmt_detail::vformat(fmt, list, sizeof...(Args));
}

} // namespace piece

#endif
