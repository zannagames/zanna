//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/common/Mangle.cpp
// Purpose: Implement unified mangling for linkable symbols derived from
//          dot-qualified names used across frontends and OOP emission.
// Key invariants:
//   - Output is lowercase ASCII and uses only [a-z0-9_].
//   - Plain C-safe identifiers remain readable unless they use the reserved prefix.
//   - Qualified or otherwise unsafe names are encoded with reversible escapes.
// Ownership/Lifetime: Stateless helpers returning std::string by value.
// Links: src/common/Mangle.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements symbol mangling helpers for linkable names.
/// @details Provides deterministic conversions between user-facing qualified
///          identifiers and ASCII-safe linker symbols. The mapping is stable
///          across platforms and is reversible for names emitted with the
///          reserved prefix.

#include "common/Mangle.hpp"

#include <algorithm>
#include <iterator>
#include <string>

namespace zanna::common {
namespace {

/// Prefix reserving the reversible escaped-symbol namespace.
constexpr std::string_view kReservedPrefix = "vpr_";

/// Prefix applied to a user symbol that would otherwise claim a C runtime name.
/// @invariant Plain, lowercase, and not @ref kReservedPrefix, so @ref MangleLink
///            maps a guarded name to itself and re-guarding is a no-op.
constexpr std::string_view kRuntimeGuardPrefix = "zn_";

/// Lowercase C and POSIX runtime symbol names that user code must never claim.
/// @details A natively linked program shares one flat symbol namespace with the
///          C runtime it links against, so a plain user symbol spelled like a
///          library function satisfies that library's own internal reference to
///          it.  A Zia `func floor` would then be called by the runtime's
///          ellipse rasterizer in place of libm's `floor`, silently
///          miscompiling the program instead of failing the link.  Names listed
///          here are therefore pushed into the reserved escaped namespace.
///          `main` is deliberately absent: it is the native entry point and must
///          keep its plain spelling.  Zanna's own runtime is unaffected because
///          every runtime export is `rt_`-prefixed.
/// @invariant Sorted ascending so lookups can use binary search.
// clang-format off: one name per line would make this table 443 lines long.
constexpr std::string_view kReservedRuntimeSymbols[] = {
    "abort", "abs", "accept", "access", "acos", "acosf", "acosh", "acoshf", "acoshl", "acosl",
    "aligned_alloc", "alloca", "arc4random", "asctime", "asin", "asinf", "asinh", "asinhf",
    "asinhl", "asinl", "at_quick_exit", "atan", "atan2", "atan2f", "atan2l", "atanf", "atanh",
    "atanhf", "atanhl", "atanl", "atexit", "atof", "atoi", "atol", "atoll", "bcmp", "bcopy",
    "bind", "bsearch", "bzero", "calloc", "cbrt", "cbrtf", "cbrtl", "ceil", "ceilf", "ceill",
    "chdir", "chmod", "chown", "clearerr", "clock", "clock_gettime", "close", "closedir",
    "connect", "copysign", "copysignf", "copysignl", "cos", "cosf", "cosh", "coshf", "coshl",
    "cosl", "creat", "ctime", "difftime", "div", "drand48", "drem", "dup", "dup2", "erf",
    "erfc", "erfcf", "erfcl", "erff", "erfl", "execl", "execle", "execlp", "execv", "execve",
    "execvp", "exit", "exp", "exp2", "exp2f", "exp2l", "expf", "expl", "expm1", "expm1f",
    "expm1l", "fabs", "fabsf", "fabsl", "fchmod", "fclose", "fcntl", "fdim", "fdimf", "fdiml",
    "fdopen", "feof", "ferror", "fflush", "fgetc", "fgetpos", "fgets", "fileno", "finite",
    "floor", "floorf", "floorl", "fma", "fmaf", "fmal", "fmax", "fmaxf", "fmaxl", "fmin",
    "fminf", "fminl", "fmod", "fmodf", "fmodl", "fopen", "fork", "fprintf", "fputc", "fputs",
    "fread", "free", "freopen", "frexp", "frexpf", "frexpl", "fscanf", "fseek", "fseeko",
    "fsetpos", "fstat", "fsync", "ftell", "ftello", "ftruncate", "fwrite", "gamma", "getc",
    "getchar", "getcwd", "getegid", "getenv", "geteuid", "getgid", "getline", "getpeername",
    "getpid", "getppid", "gets", "getsockname", "getsockopt", "gettimeofday", "getuid",
    "gmtime", "htonl", "htons", "hypot", "hypotf", "hypotl", "ilogb", "ilogbf", "ilogbl",
    "index", "initstate", "ioctl", "isalnum", "isalpha", "isatty", "isblank", "iscntrl",
    "isdigit", "isgraph", "islower", "isprint", "ispunct", "isspace", "isupper", "isxdigit",
    "j0", "j1", "jn", "kill", "labs", "ldexp", "ldexpf", "ldexpl", "ldiv", "lgamma", "lgammaf",
    "lgammal", "link", "listen", "llabs", "lldiv", "llrint", "llrintf", "llrintl", "llround",
    "llroundf", "llroundl", "localeconv", "localtime", "log", "log10", "log10f", "log10l",
    "log1p", "log1pf", "log1pl", "log2", "log2f", "log2l", "logb", "logbf", "logbl", "logf",
    "logl", "longjmp", "lrand48", "lrint", "lrintf", "lrintl", "lround", "lroundf", "lroundl",
    "lseek", "lstat", "malloc", "mblen", "mbstowcs", "mbtowc", "memccpy", "memchr", "memcmp",
    "memcpy", "memmove", "mempcpy", "memset", "mkdir", "mkfifo", "mknod", "mkstemp", "mktemp",
    "mktime", "mmap", "modf", "modff", "modfl", "munmap", "nan", "nanf", "nanl", "nanosleep",
    "nearbyint", "nearbyintf", "nearbyintl", "nextafter", "nextafterf", "nextafterl",
    "nexttoward", "nexttowardf", "nexttowardl", "ntohl", "ntohs", "open", "opendir", "pclose",
    "perror", "pipe", "poll", "popen", "posix_memalign", "pow", "powf", "powl", "pread",
    "printf", "putc", "putchar", "putenv", "puts", "pwrite", "qsort", "quick_exit", "raise",
    "rand", "random", "read", "readdir", "readlink", "realloc", "realpath", "recv", "recvfrom",
    "remainder", "remainderf", "remainderl", "remove", "remquo", "remquof", "remquol", "rename",
    "rewind", "rindex", "rint", "rintf", "rintl", "rmdir", "round", "roundf", "roundl", "sbrk",
    "scalbln", "scalblnf", "scalblnl", "scalbn", "scalbnf", "scalbnl", "scanf", "select",
    "send", "sendto", "setbuf", "setenv", "setjmp", "setlocale", "setsockopt", "setstate",
    "setvbuf", "shutdown", "siglongjmp", "signal", "significand", "sigsetjmp", "sin", "sinf",
    "sinh", "sinhf", "sinhl", "sinl", "sleep", "snprintf", "socket", "socketpair", "sprintf",
    "sqrt", "sqrtf", "sqrtl", "srand", "srand48", "srandom", "sscanf", "stat", "stpcpy",
    "strcasecmp", "strcat", "strchr", "strcmp", "strcoll", "strcpy", "strcspn", "strdup",
    "strerror", "strftime", "strlcat", "strlcpy", "strlen", "strncasecmp", "strncat", "strncmp",
    "strncpy", "strndup", "strnlen", "strpbrk", "strrchr", "strsep", "strspn", "strstr",
    "strtod", "strtof", "strtok", "strtol", "strtold", "strtoll", "strtoul", "strtoull",
    "strxfrm", "symlink", "sync", "sysconf", "system", "tan", "tanf", "tanh", "tanhf", "tanhl",
    "tanl", "tgamma", "tgammaf", "tgammal", "time", "tmpfile", "tmpnam", "tolower", "toupper",
    "trunc", "truncf", "truncl", "umask", "ungetc", "unlink", "unsetenv", "usleep", "utime",
    "valloc", "vfprintf", "vfscanf", "vprintf", "vscanf", "vsnprintf", "vsprintf", "vsscanf",
    "wait", "waitpid", "wcstombs", "wctomb", "write", "y0", "y1", "yn",
};
// clang-format on

/// @brief Return whether @p normalized names a C or POSIX runtime symbol.
/// @param normalized Candidate symbol after ASCII case folding.
/// @return True when the name appears in @ref kReservedRuntimeSymbols.
[[nodiscard]] bool is_reserved_runtime_symbol(std::string_view normalized) noexcept {
    const auto *first = std::begin(kReservedRuntimeSymbols);
    const auto *last = std::end(kReservedRuntimeSymbols);
    return std::binary_search(first, last, normalized);
}

/// @brief Return whether @p ch is an ASCII decimal digit.
/// @param ch Byte to classify.
/// @return True when @p ch is in the range @c '0' through @c '9'.
[[nodiscard]] bool is_ascii_digit(unsigned char ch) noexcept {
    return ch >= '0' && ch <= '9';
}

/// @brief Return whether @p ch is an ASCII lowercase letter.
/// @param ch Byte to classify.
/// @return True when @p ch is in the range @c 'a' through @c 'z'.
[[nodiscard]] bool is_ascii_lower(unsigned char ch) noexcept {
    return ch >= 'a' && ch <= 'z';
}

/// @brief Convert one ASCII byte to lowercase without locale dependence.
/// @param ch Byte to normalize.
/// @return Lowercase ASCII byte when @p ch is @c A-Z, otherwise @p ch unchanged.
[[nodiscard]] unsigned char ascii_lower(unsigned char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<unsigned char>(ch - 'A' + 'a');
    }
    return ch;
}

/// @brief Return whether @p normalized is a plain symbol that can remain unescaped.
/// @details Plain symbols are readable linker identifiers whose lowercase spelling
///          does not begin with the reserved escape prefix.
/// @param normalized Candidate after ASCII case folding; non-ASCII and other
///                   unsupported bytes cause the check to fail.
/// @return True when @p normalized can be emitted without the reserved encoding.
[[nodiscard]] bool can_emit_plain(std::string_view normalized) noexcept {
    if (normalized.empty() || normalized.rfind(kReservedPrefix, 0) == 0) {
        return false;
    }
    const auto first = static_cast<unsigned char>(normalized.front());
    if (!is_ascii_lower(first) && first != '_') {
        return false;
    }
    for (const unsigned char ch : normalized) {
        if (!is_ascii_lower(ch) && !is_ascii_digit(ch) && ch != '_') {
            return false;
        }
    }
    return true;
}

/// @brief Append a two-character lowercase hexadecimal byte escape.
/// @param out Destination symbol buffer.
/// @param ch Byte to encode after an @c _x escape introducer.
/// @post Exactly two lowercase hexadecimal characters are appended to @p out.
void append_hex_byte(std::string &out, unsigned char ch) {
    constexpr char digits[] = "0123456789abcdef";
    out.push_back(digits[(ch >> 4U) & 0x0FU]);
    out.push_back(digits[ch & 0x0FU]);
}

/// @brief Decode one lowercase hexadecimal digit.
/// @param ch ASCII byte to decode.
/// @return Value in the range 0-15, or -1 when @p ch is not hexadecimal.
[[nodiscard]] int decode_hex_digit(unsigned char ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return static_cast<int>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<int>(ch - 'a' + 10);
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<int>(ch - 'A' + 10);
    }
    return -1;
}

} // namespace

/// @copydoc MangleLink()
std::string MangleLink(std::string_view qualified) {
    std::string normalized;
    normalized.reserve(qualified.size());
    for (unsigned char ch : qualified) {
        normalized.push_back(static_cast<char>(ascii_lower(ch)));
    }

    if (can_emit_plain(normalized)) {
        return normalized;
    }

    std::string out;
    out.reserve(kReservedPrefix.size() + normalized.size() * 4U);
    out.append(kReservedPrefix);
    for (unsigned char ch : normalized) {
        if (is_ascii_lower(ch) || is_ascii_digit(ch)) {
            out.push_back(static_cast<char>(ch));
        } else if (ch == '.') {
            out.append("_d");
        } else if (ch == '_') {
            out.append("_u");
        } else {
            out.append("_x");
            append_hex_byte(out, ch);
        }
    }
    return out;
}

/// @copydoc IsReservedRuntimeName()
bool IsReservedRuntimeName(std::string_view name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (unsigned char ch : name) {
        normalized.push_back(static_cast<char>(ascii_lower(ch)));
    }
    return is_reserved_runtime_symbol(normalized);
}

/// @copydoc GuardReservedRuntimeName()
std::string GuardReservedRuntimeName(std::string_view name) {
    if (!IsReservedRuntimeName(name)) {
        return std::string(name);
    }
    std::string out;
    out.reserve(kRuntimeGuardPrefix.size() + name.size());
    out.append(kRuntimeGuardPrefix);
    for (unsigned char ch : name) {
        out.push_back(static_cast<char>(ascii_lower(ch)));
    }
    return out;
}

/// @copydoc DemangleLink()
std::string DemangleLink(std::string_view symbol) {
    if (symbol.rfind(kReservedPrefix, 0) == 0) {
        std::string out;
        std::string_view body = symbol.substr(kReservedPrefix.size());
        out.reserve(body.size());
        for (std::size_t i = 0; i < body.size(); ++i) {
            const unsigned char ch = static_cast<unsigned char>(body[i]);
            if (ch != '_') {
                out.push_back(static_cast<char>(ch));
                continue;
            }
            if (i + 1 >= body.size()) {
                out.push_back('_');
                continue;
            }
            const unsigned char code = static_cast<unsigned char>(body[++i]);
            if (code == 'd') {
                out.push_back('.');
            } else if (code == 'u') {
                out.push_back('_');
            } else if (code == 'x' && i + 2 < body.size()) {
                const int hi = decode_hex_digit(static_cast<unsigned char>(body[i + 1]));
                const int lo = decode_hex_digit(static_cast<unsigned char>(body[i + 2]));
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                } else {
                    out.append("_x");
                }
            } else {
                out.push_back('_');
                out.push_back(static_cast<char>(code));
            }
        }
        return out;
    }

    std::string_view body = symbol;
    if (!body.empty() && body.front() == '@') {
        body.remove_prefix(1);
        std::string out;
        out.reserve(body.size());
        for (unsigned char ch : body) {
            out.push_back(ch == '_' ? '.' : static_cast<char>(ch));
        }
        return out;
    }

    std::string out;
    out.reserve(body.size());
    for (unsigned char ch : body) {
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

} // namespace zanna::common
