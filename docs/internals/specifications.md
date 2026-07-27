---
status: active
audience: internal
last-verified: 2026-07-26
---

# Zanna C++ Compiler Specifications

This document provides the complete specification for the Zanna C++ system compiler.

## Scope

| Aspect | Specification |
|--------|---------------|
| **Host Platforms** | macOS (ARM64), Linux (x86-64, ARM64) |
| **Target Platform** | AArch64 only |
| **C++ Standard** | C++20 |
| **C Standard** | C17 |

The compiler is a **cross-compiler**: it runs on macOS/Linux development machines and generates code exclusively for AArch64 targets (Zanna OS).

---

## Table of Contents

1. [Language Standards](#1-language-standards)
2. [C++ Language Features](#2-c-language-features)
3. [C Language Features](#3-c-language-features)
4. [Type System](#4-type-system)
5. [C++ Standard Library](#5-c-standard-library)
6. [C Standard Library](#6-c-standard-library)
7. [Compiler Builtins](#7-compiler-builtins)
8. [Attributes](#8-attributes)
9. [Preprocessor](#9-preprocessor)
11. [AArch64 Architecture Support](#11-aarch64-architecture-support)
12. [Platform and ABI](#12-platform-and-abi)
13. [Compiler Flags](#13-compiler-flags)
14. [Linker Requirements](#14-linker-requirements)

---

## 1. Language Standards

| Standard | Version |
|----------|---------|
| C++ | C++20 |
| C | C17 |

---

## 2. C++ Language Features

### 2.1 Type Deduction

- `auto` variable declarations
- `auto` return types
- `decltype(expr)`
- `decltype(auto)`

### 2.2 Constants and Compile-Time

- `constexpr` variables
- `constexpr` functions
- `static_assert(condition, message)`
- `static_assert(condition)` (C++17)

### 2.3 References and Pointers

- Lvalue references (`T&`)
- Rvalue references (`T&&`)
- `nullptr` keyword
- `std::nullptr_t` type

### 2.4 Initialization

- Uniform initialization (`Type{args}`)
- Initializer lists (`{a, b, c}`)
- In-class member initializers
- Constructor initializer lists
- Designated initializers (C++20)

### 2.5 Control Flow

- Range-based for loops (`for (auto& x : container)`)
- `if` with initializer (`if (auto x = f(); cond)`)
- Structured bindings (`auto [a, b] = expr`)

### 2.6 Functions

- Default arguments
- Variadic functions
- `noexcept` specifier
- `noexcept(expr)` conditional
- Trailing return types (`auto f() -> T`)
- Deleted functions (`= delete`)
- Defaulted functions (`= default`)

### 2.7 Lambda Expressions

- Basic lambdas (`[]() {}`)
- Capture by value (`[=]`)
- Capture by reference (`[&]`)
- Explicit captures (`[x, &y]`)
- Init captures (`[x = std::move(y)]`)
- `this` capture (`[this]`, `[*this]`)
- Generic lambdas (`[](auto x) {}`)
- `mutable` lambdas

### 2.8 Classes

- Class and struct definitions
- Access specifiers (`public`, `private`, `protected`)
- Constructors and destructors
- Copy constructor and assignment
- Move constructor and assignment
- `explicit` constructors
- Conversion operators (`explicit operator bool()`)
- `virtual` functions
- Pure virtual functions (`= 0`)
- `override` specifier
- `final` specifier (classes and methods)
- Multiple inheritance
- Virtual inheritance
- Friend declarations
- Nested classes
- Local classes
- Anonymous classes/structs

### 2.9 Templates

- Class templates
- Function templates
- Variable templates
- Alias templates (`template<class T> using X = Y<T>`)
- Non-type template parameters
- Template parameter packs (`template<typename... Ts>`)
- Pack expansion (`Ts...`)
- Fold expressions (`(args + ...)`)
- Full template specialization
- Partial template specialization
- SFINAE
- `if constexpr`
- Deduction guides (CTAD)

### 2.10 Namespaces

- Namespace declarations
- Nested namespaces (`namespace a::b::c`)
- Anonymous namespaces
- Inline namespaces
- `using` declarations
- `using namespace` directives
- Namespace aliases

### 2.11 Enumerations

- Unscoped enums (`enum E`)
- Scoped enums (`enum class E`)
- Enum with underlying type (`enum class E : uint8_t`)
- Forward declarations

### 2.12 Unions

- Union definitions
- Anonymous unions

### 2.13 Operators

- All arithmetic operators (`+`, `-`, `*`, `/`, `%`)
- All bitwise operators (`&`, `|`, `^`, `~`, `<<`, `>>`)
- All comparison operators (`==`, `!=`, `<`, `>`, `<=`, `>=`)
- All logical operators (`&&`, `||`, `!`)
- All assignment operators (`=`, `+=`, `-=`, etc.)
- Increment/decrement (`++`, `--`)
- Member access (`.`, `->`, `.*`, `->*`)
- Subscript (`[]`)
- Function call (`()`)
- Comma (`,`)
- Ternary (`?:`)
- `sizeof`, `alignof`
- `typeid`
- `new`, `delete`, `new[]`, `delete[]`
- Placement new
- User-defined operators
- Operator overloading

### 2.14 Casts

- `static_cast<T>(expr)`
- `dynamic_cast<T>(expr)`
- `const_cast<T>(expr)`
- `reinterpret_cast<T>(expr)`
- C-style casts (`(T)expr`)
- Functional casts (`T(expr)`)

### 2.15 Exception Handling

- `try` / `catch` / `throw`
- `noexcept` specifier
- Exception specifications (deprecated)
- `std::exception` hierarchy

### 2.16 Miscellaneous

- `volatile` qualifier
- `mutable` members
- `extern "C"` linkage
- `thread_local` storage
- Inline variables
- Inline functions
- `alignas` specifier

---

## 3. C Language Features

### 3.1 C11 Features

- `//` comments
- Mixed declarations and code
- `inline` functions
- `restrict` keyword
- Designated initializers
- Compound literals
- Variable-length arrays (VLA)
- Flexible array members
- `_Bool` / `bool`
- `long long` type
- `snprintf`
- `__func__` predefined identifier

### 3.2 C11 Features

- `_Static_assert`
- `_Alignas` / `_Alignof`
- `_Noreturn`
- `_Generic` selection
- Anonymous structs and unions
- `<stdatomic.h>` support

### 3.3 C17 Features

- `__has_include` preprocessor
- Deprecated features removed

---

## 4. Type System

### 4.1 Fundamental Types

| Category | Types |
|----------|-------|
| Boolean | `bool` |
| Character | `char`, `signed char`, `unsigned char`, `wchar_t`, `char8_t`, `char16_t`, `char32_t` |
| Integer | `short`, `int`, `long`, `long long` (signed and unsigned) |
| Floating | `float`, `double`, `long double` |
| Void | `void` |
| Nullptr | `std::nullptr_t` |

### 4.2 Fixed-Width Integer Types

| Signed | Unsigned |
|--------|----------|
| `int8_t` | `uint8_t` |
| `int16_t` | `uint16_t` |
| `int32_t` | `uint32_t` |
| `int64_t` | `uint64_t` |
| `int_least8_t` | `uint_least8_t` |
| `int_least16_t` | `uint_least16_t` |
| `int_least32_t` | `uint_least32_t` |
| `int_least64_t` | `uint_least64_t` |
| `int_fast8_t` | `uint_fast8_t` |
| `int_fast16_t` | `uint_fast16_t` |
| `int_fast32_t` | `uint_fast32_t` |
| `int_fast64_t` | `uint_fast64_t` |
| `intmax_t` | `uintmax_t` |
| `intptr_t` | `uintptr_t` |

### 4.3 Size and Pointer Types

- `size_t`
- `ssize_t`
- `ptrdiff_t`
- `max_align_t`
- `nullptr_t`

### 4.4 Compound Types

- Pointers (`T*`)
- References (`T&`, `T&&`)
- Arrays (`T[N]`, `T[]`)
- Functions (`R(Args...)`)
- Pointers to members (`T C::*`)
- Pointers to member functions (`R (C::*)(Args...)`)

### 4.5 Type Qualifiers

- `const`
- `volatile`
- `const volatile`

---

## 5. C++ Standard Library

### 5.1 Containers

| Header | Components |
|--------|------------|
| `<vector>` | `std::vector<T>` |
| `<array>` | `std::array<T, N>` |
| `<string>` | `std::string`, `std::wstring` |
| `<string_view>` | `std::string_view` |
| `<map>` | `std::map<K, V>`, `std::multimap<K, V>` |
| `<set>` | `std::set<T>`, `std::multiset<T>` |
| `<unordered_map>` | `std::unordered_map<K, V>`, `std::unordered_multimap<K, V>` |
| `<unordered_set>` | `std::unordered_set<T>`, `std::unordered_multiset<T>` |
| `<deque>` | `std::deque<T>` |
| `<list>` | `std::list<T>` |
| `<forward_list>` | `std::forward_list<T>` |
| `<queue>` | `std::queue<T>`, `std::priority_queue<T>` |
| `<stack>` | `std::stack<T>` |
| `<span>` | `std::span<T>` |
| `<bitset>` | `std::bitset<N>` |

### 5.2 Utilities

| Header | Components |
|--------|------------|
| `<utility>` | `std::pair`, `std::move`, `std::forward`, `std::swap`, `std::exchange` |
| `<tuple>` | `std::tuple`, `std::get`, `std::make_tuple`, `std::tie` |
| `<optional>` | `std::optional<T>`, `std::nullopt` |
| `<variant>` | `std::variant<Ts...>`, `std::get`, `std::get_if`, `std::holds_alternative`, `std::visit` |
| `<any>` | `std::any`, `std::any_cast` |
| `<functional>` | `std::function<Sig>`, `std::invoke`, `std::reference_wrapper`, `std::bind` |
| `<memory>` | `std::unique_ptr<T>`, `std::shared_ptr<T>`, `std::weak_ptr<T>`, `std::make_unique`, `std::make_shared`, `std::allocator<T>` |
| `<initializer_list>` | `std::initializer_list<T>` |

### 5.3 Algorithms

| Header | Components |
|--------|------------|
| `<algorithm>` | `std::sort`, `std::find`, `std::copy`, `std::transform`, `std::for_each`, `std::remove_if`, `std::unique`, `std::reverse`, `std::fill`, `std::count`, `std::min`, `std::max`, `std::clamp` |
| `<numeric>` | `std::accumulate`, `std::iota`, `std::gcd`, `std::lcm`, `std::reduce` |

### 5.4 Iterators

| Header | Components |
|--------|------------|
| `<iterator>` | `std::begin`, `std::end`, `std::next`, `std::prev`, `std::advance`, `std::distance`, `std::back_inserter`, `std::front_inserter`, `std::inserter`, iterator traits |

### 5.5 Type Traits

| Header | Components |
|--------|------------|
| `<type_traits>` | `std::enable_if`, `std::enable_if_t`, `std::conditional`, `std::conditional_t`, `std::is_same`, `std::is_same_v`, `std::is_integral`, `std::is_integral_v`, `std::is_floating_point`, `std::is_pointer`, `std::is_reference`, `std::is_const`, `std::is_trivially_copyable`, `std::is_trivially_destructible`, `std::remove_cv`, `std::remove_cv_t`, `std::remove_reference`, `std::remove_reference_t`, `std::remove_pointer`, `std::decay`, `std::decay_t`, `std::add_const`, `std::add_pointer`, `std::underlying_type`, `std::void_t` |
| `<limits>` | `std::numeric_limits<T>` |

### 5.6 Strings and I/O

| Header | Components |
|--------|------------|
| `<string>` | `std::string`, `std::to_string`, `std::stoi`, `std::stol`, `std::stod` |
| `<string_view>` | `std::string_view` |
| `<charconv>` | `std::to_chars`, `std::from_chars` |
| `<sstream>` | `std::stringstream`, `std::ostringstream`, `std::istringstream` |
| `<iostream>` | `std::cout`, `std::cerr`, `std::cin`, `std::endl` |
| `<fstream>` | `std::ifstream`, `std::ofstream`, `std::fstream` |
| `<iomanip>` | `std::setw`, `std::setfill`, `std::hex`, `std::dec`, `std::setprecision` |

### 5.7 Concurrency

| Header | Components |
|--------|------------|
| `<thread>` | `std::thread`, `std::this_thread::sleep_for`, `std::this_thread::yield` |
| `<mutex>` | `std::mutex`, `std::recursive_mutex`, `std::lock_guard`, `std::unique_lock`, `std::scoped_lock` |
| `<condition_variable>` | `std::condition_variable` |
| `<atomic>` | `std::atomic<T>`, `std::atomic_flag`, memory orders |
| `<future>` | `std::future`, `std::promise`, `std::async`, `std::packaged_task` |

### 5.8 Time

| Header | Components |
|--------|------------|
| `<chrono>` | `std::chrono::duration`, `std::chrono::time_point`, `std::chrono::system_clock`, `std::chrono::steady_clock`, `std::chrono::high_resolution_clock`, duration literals |

### 5.9 Error Handling

| Header | Components |
|--------|------------|
| `<exception>` | `std::exception`, `std::terminate`, `std::current_exception`, `std::rethrow_exception` |
| `<stdexcept>` | `std::runtime_error`, `std::logic_error`, `std::out_of_range`, `std::invalid_argument`, `std::overflow_error` |
| `<system_error>` | `std::error_code`, `std::error_category`, `std::system_error` |

### 5.10 C Compatibility Headers

| C++ Header | C Header |
|------------|----------|
| `<cstdint>` | `<stdint.h>` |
| `<cstddef>` | `<stddef.h>` |
| `<cstdlib>` | `<stdlib.h>` |
| `<cstdio>` | `<stdio.h>` |
| `<cstring>` | `<string.h>` |
| `<cmath>` | `<math.h>` |
| `<cassert>` | `<assert.h>` |
| `<cerrno>` | `<errno.h>` |
| `<climits>` | `<limits.h>` |
| `<cfloat>` | `<float.h>` |
| `<cctype>` | `<ctype.h>` |
| `<ctime>` | `<time.h>` |
| `<csignal>` | `<signal.h>` |
| `<csetjmp>` | `<setjmp.h>` |
| `<cstdarg>` | `<stdarg.h>` |
| `<cinttypes>` | `<inttypes.h>` |

---

## 6. C Standard Library

### 6.1 Core Headers

| Header | Key Functions/Macros |
|--------|---------------------|
| `<stdio.h>` | `printf`, `fprintf`, `sprintf`, `snprintf`, `vprintf`, `vfprintf`, `vsprintf`, `vsnprintf`, `scanf`, `sscanf`, `fopen`, `fclose`, `fread`, `fwrite`, `fgets`, `fputs`, `fputc`, `fgetc`, `fseek`, `ftell`, `rewind`, `fflush`, `feof`, `ferror`, `perror`, `remove`, `rename`, `tmpfile`, `tmpnam`, `fileno`, `fdopen`, `freopen`, `getchar`, `putchar`, `puts` |
| `<stdlib.h>` | `malloc`, `calloc`, `realloc`, `free`, `atoi`, `atol`, `atoll`, `atof`, `strtol`, `strtoul`, `strtoll`, `strtoull`, `strtod`, `strtof`, `strtold`, `abs`, `labs`, `llabs`, `div`, `ldiv`, `lldiv`, `exit`, `_Exit`, `abort`, `atexit`, `getenv`, `setenv`, `unsetenv`, `system`, `qsort`, `bsearch`, `rand`, `srand` |
| `<string.h>` | `strlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr`, `strpbrk`, `strspn`, `strcspn`, `strtok`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strerror` |
| `<stdint.h>` | All fixed-width types, `INT*_MIN`, `INT*_MAX`, `UINT*_MAX`, `SIZE_MAX`, `INTPTR_MIN`, `INTPTR_MAX` |
| `<stddef.h>` | `size_t`, `ptrdiff_t`, `NULL`, `offsetof`, `max_align_t` |
| `<stdbool.h>` | `bool`, `true`, `false` |
| `<stdarg.h>` | `va_list`, `va_start`, `va_arg`, `va_end`, `va_copy` |

### 6.2 Additional Headers

| Header | Key Functions/Macros |
|--------|---------------------|
| `<limits.h>` | `CHAR_BIT`, `CHAR_MIN`, `CHAR_MAX`, `SCHAR_MIN`, `SCHAR_MAX`, `UCHAR_MAX`, `SHRT_MIN`, `SHRT_MAX`, `USHRT_MAX`, `INT_MIN`, `INT_MAX`, `UINT_MAX`, `LONG_MIN`, `LONG_MAX`, `ULONG_MAX`, `LLONG_MIN`, `LLONG_MAX`, `ULLONG_MAX` |
| `<float.h>` | `FLT_MIN`, `FLT_MAX`, `FLT_EPSILON`, `DBL_MIN`, `DBL_MAX`, `DBL_EPSILON`, `LDBL_MIN`, `LDBL_MAX`, `LDBL_EPSILON`, `FLT_DIG`, `DBL_DIG`, `FLT_MANT_DIG`, `DBL_MANT_DIG` |
| `<errno.h>` | `errno`, `EDOM`, `ERANGE`, `EILSEQ`, `ENOENT`, `EINVAL`, `ENOMEM`, `EBADF`, `EACCES`, `EEXIST`, `ETIMEDOUT`, `EINTR`, `EAGAIN`, `ENOSYS` |
| `<assert.h>` | `assert(expr)`, `static_assert` |
| `<ctype.h>` | `isalpha`, `isdigit`, `isalnum`, `isspace`, `isupper`, `islower`, `isprint`, `ispunct`, `iscntrl`, `isxdigit`, `isgraph`, `isblank`, `toupper`, `tolower` |
| `<math.h>` | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `exp`, `exp2`, `log`, `log10`, `log2`, `pow`, `sqrt`, `cbrt`, `hypot`, `ceil`, `floor`, `round`, `trunc`, `fmod`, `remainder`, `fabs`, `fmax`, `fmin`, `copysign`, `NAN`, `INFINITY`, `isnan`, `isinf`, `isfinite`, `fpclassify` |
| `<time.h>` | `time`, `clock`, `difftime`, `mktime`, `localtime`, `gmtime`, `strftime`, `clock_gettime`, `CLOCK_REALTIME`, `CLOCK_MONOTONIC` |
| `<signal.h>` | `signal`, `raise`, `SIGINT`, `SIGTERM`, `SIGSEGV`, `SIGABRT`, `SIGFPE`, `SIGILL` |
| `<setjmp.h>` | `setjmp`, `longjmp`, `jmp_buf` |
| `<inttypes.h>` | `PRId8`, `PRId16`, `PRId32`, `PRId64`, `PRIu8`, `PRIu16`, `PRIu32`, `PRIu64`, `PRIx8`, `PRIx16`, `PRIx32`, `PRIx64`, `SCNd32`, `SCNu64`, `strtoimax`, `strtoumax`, `imaxdiv` |
| `<fenv.h>` | `feclearexcept`, `fegetexceptflag`, `feraiseexcept`, `fesetexceptflag`, `fetestexcept`, `fegetround`, `fesetround`, `fegetenv`, `fesetenv` |
| `<locale.h>` | `setlocale`, `localeconv`, `LC_ALL`, `LC_COLLATE`, `LC_CTYPE`, `LC_MONETARY`, `LC_NUMERIC`, `LC_TIME` |
| `<wchar.h>` | `wprintf`, `fwprintf`, `swprintf`, `wcslen`, `wcscpy`, `wcsncpy`, `wcscat`, `wcscmp`, `wcschr`, `wcsrchr`, `wcsstr`, `wmemcpy`, `wmemset`, `wmemmove`, `wmemcmp` |

### 6.3 POSIX Extensions

| Header | Key Functions |
|--------|---------------|
| `<unistd.h>` | `read`, `write`, `close`, `lseek`, `unlink`, `access`, `getcwd`, `chdir`, `sleep`, `usleep`, `sbrk`, `fork`, `exec*`, `pipe`, `dup`, `dup2` |
| `<fcntl.h>` | `open`, `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`, `O_EXCL`, `O_NONBLOCK` |
| `<sys/types.h>` | `off_t`, `ssize_t`, `mode_t`, `pid_t`, `uid_t`, `gid_t` |
| `<sys/stat.h>` | `stat`, `fstat`, `lstat`, `mkdir`, `chmod`, `S_ISREG`, `S_ISDIR`, `S_IRUSR`, `S_IWUSR`, `S_IXUSR` |
| `<dirent.h>` | `opendir`, `readdir`, `closedir`, `rewinddir`, `DIR`, `struct dirent` |
| `<pthread.h>` | `pthread_create`, `pthread_join`, `pthread_detach`, `pthread_self`, `pthread_equal`, `pthread_exit`, `pthread_mutex_*`, `pthread_cond_*`, `pthread_rwlock_*`, `pthread_key_*` |
| `<sys/socket.h>` | `socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `sendto`, `recvfrom`, `shutdown`, `setsockopt`, `getsockopt` |
| `<netinet/in.h>` | `struct sockaddr_in`, `struct sockaddr_in6`, `htons`, `htonl`, `ntohs`, `ntohl`, `INADDR_ANY` |
| `<arpa/inet.h>` | `inet_addr`, `inet_ntoa`, `inet_pton`, `inet_ntop` |
| `<netdb.h>` | `gethostbyname`, `getaddrinfo`, `freeaddrinfo`, `gai_strerror` |
| `<sys/mman.h>` | `mmap`, `munmap`, `mprotect`, `PROT_READ`, `PROT_WRITE`, `PROT_EXEC`, `MAP_PRIVATE`, `MAP_SHARED`, `MAP_ANONYMOUS` |
| `<dlfcn.h>` | `dlopen`, `dlsym`, `dlclose`, `dlerror` |
| `<termios.h>` | `tcgetattr`, `tcsetattr`, `cfmakeraw`, `TCSANOW`, `TCSADRAIN` |
| `<poll.h>` | `poll`, `struct pollfd`, `POLLIN`, `POLLOUT`, `POLLERR` |
| `<sys/select.h>` | `select`, `FD_SET`, `FD_CLR`, `FD_ISSET`, `FD_ZERO` |
| `<sys/wait.h>` | `wait`, `waitpid`, `WIFEXITED`, `WEXITSTATUS`, `WIFSIGNALED` |

---

## 7. Compiler Builtins

### 7.1 Variadic Argument Builtins

| Builtin | Signature |
|---------|-----------|
| `__builtin_va_list` | Type |
| `__builtin_va_start` | `void __builtin_va_start(va_list ap, last_param)` |
| `__builtin_va_arg` | `type __builtin_va_arg(va_list ap, type)` |
| `__builtin_va_end` | `void __builtin_va_end(va_list ap)` |
| `__builtin_va_copy` | `void __builtin_va_copy(va_list dest, va_list src)` |

### 7.2 Type Introspection Builtins

| Builtin | Signature |
|---------|-----------|
| `__builtin_offsetof` | `size_t __builtin_offsetof(type, member)` |
| `__builtin_types_compatible_p` | `int __builtin_types_compatible_p(type1, type2)` |
| `__builtin_constant_p` | `int __builtin_constant_p(expr)` |

### 7.3 Control Flow Builtins

| Builtin | Signature |
|---------|-----------|
| `__builtin_unreachable` | `void __builtin_unreachable(void)` |
| `__builtin_expect` | `long __builtin_expect(long exp, long c)` |
| `__builtin_assume` | `void __builtin_assume(bool cond)` |

### 7.4 Bit Manipulation Builtins

| Builtin | Signature |
|---------|-----------|
| `__builtin_clz` | `int __builtin_clz(unsigned int x)` |
| `__builtin_clzl` | `int __builtin_clzl(unsigned long x)` |
| `__builtin_clzll` | `int __builtin_clzll(unsigned long long x)` |
| `__builtin_ctz` | `int __builtin_ctz(unsigned int x)` |
| `__builtin_ctzl` | `int __builtin_ctzl(unsigned long x)` |
| `__builtin_ctzll` | `int __builtin_ctzll(unsigned long long x)` |
| `__builtin_popcount` | `int __builtin_popcount(unsigned int x)` |
| `__builtin_popcountl` | `int __builtin_popcountl(unsigned long x)` |
| `__builtin_popcountll` | `int __builtin_popcountll(unsigned long long x)` |
| `__builtin_parity` | `int __builtin_parity(unsigned int x)` |
| `__builtin_ffs` | `int __builtin_ffs(int x)` |
| `__builtin_bswap16` | `uint16_t __builtin_bswap16(uint16_t x)` |
| `__builtin_bswap32` | `uint32_t __builtin_bswap32(uint32_t x)` |
| `__builtin_bswap64` | `uint64_t __builtin_bswap64(uint64_t x)` |

### 7.5 Arithmetic Builtins

| Builtin | Signature |
|---------|-----------|
| `__builtin_add_overflow` | `bool __builtin_add_overflow(T a, T b, T *res)` |
| `__builtin_sub_overflow` | `bool __builtin_sub_overflow(T a, T b, T *res)` |
| `__builtin_mul_overflow` | `bool __builtin_mul_overflow(T a, T b, T *res)` |
| `__builtin_sadd_overflow` | `bool __builtin_sadd_overflow(int a, int b, int *res)` |
| `__builtin_uadd_overflow` | `bool __builtin_uadd_overflow(unsigned a, unsigned b, unsigned *res)` |

### 7.6 Memory Builtins

| Builtin | Signature |
|---------|-----------|
| `__builtin_memcpy` | `void *__builtin_memcpy(void *dest, const void *src, size_t n)` |
| `__builtin_memset` | `void *__builtin_memset(void *s, int c, size_t n)` |
| `__builtin_memmove` | `void *__builtin_memmove(void *dest, const void *src, size_t n)` |
| `__builtin_memcmp` | `int __builtin_memcmp(const void *s1, const void *s2, size_t n)` |
| `__builtin_alloca` | `void *__builtin_alloca(size_t size)` |
| `__builtin_prefetch` | `void __builtin_prefetch(const void *addr, ...)` |

### 7.7 Floating-Point Builtins

| Builtin | Signature |
|---------|-----------|
| `__builtin_inf` | `double __builtin_inf(void)` |
| `__builtin_inff` | `float __builtin_inff(void)` |
| `__builtin_nan` | `double __builtin_nan(const char *str)` |
| `__builtin_nanf` | `float __builtin_nanf(const char *str)` |
| `__builtin_huge_val` | `double __builtin_huge_val(void)` |
| `__builtin_huge_valf` | `float __builtin_huge_valf(void)` |
| `__builtin_isnan` | `int __builtin_isnan(x)` |
| `__builtin_isinf` | `int __builtin_isinf(x)` |
| `__builtin_isfinite` | `int __builtin_isfinite(x)` |

### 7.8 Address Builtins

| Builtin | Signature |
|---------|-----------|
| `__builtin_return_address` | `void *__builtin_return_address(unsigned int level)` |
| `__builtin_frame_address` | `void *__builtin_frame_address(unsigned int level)` |

### 7.9 Atomic Builtins

| Builtin | Signature |
|---------|-----------|
| `__atomic_load_n` | `T __atomic_load_n(T *ptr, int memorder)` |
| `__atomic_store_n` | `void __atomic_store_n(T *ptr, T val, int memorder)` |
| `__atomic_exchange_n` | `T __atomic_exchange_n(T *ptr, T val, int memorder)` |
| `__atomic_compare_exchange_n` | `bool __atomic_compare_exchange_n(T *ptr, T *expected, T desired, bool weak, int success_memorder, int failure_memorder)` |
| `__atomic_fetch_add` | `T __atomic_fetch_add(T *ptr, T val, int memorder)` |
| `__atomic_fetch_sub` | `T __atomic_fetch_sub(T *ptr, T val, int memorder)` |
| `__atomic_fetch_and` | `T __atomic_fetch_and(T *ptr, T val, int memorder)` |
| `__atomic_fetch_or` | `T __atomic_fetch_or(T *ptr, T val, int memorder)` |
| `__atomic_fetch_xor` | `T __atomic_fetch_xor(T *ptr, T val, int memorder)` |
| `__atomic_thread_fence` | `void __atomic_thread_fence(int memorder)` |
| `__atomic_signal_fence` | `void __atomic_signal_fence(int memorder)` |

**Memory Order Constants:**
- `__ATOMIC_RELAXED`
- `__ATOMIC_CONSUME`
- `__ATOMIC_ACQUIRE`
- `__ATOMIC_RELEASE`
- `__ATOMIC_ACQ_REL`
- `__ATOMIC_SEQ_CST`

---

## 8. Attributes

### 8.1 C++ Standard Attributes

| Attribute | Syntax |
|-----------|--------|
| noreturn | `[[noreturn]]` |
| nodiscard | `[[nodiscard]]` |
| nodiscard with message | `[[nodiscard("message")]]` |
| maybe_unused | `[[maybe_unused]]` |
| deprecated | `[[deprecated]]` |
| deprecated with message | `[[deprecated("message")]]` |
| fallthrough | `[[fallthrough]]` |
| likely | `[[likely]]` |
| unlikely | `[[unlikely]]` |
| no_unique_address | `[[no_unique_address]]` |
| carries_dependency | `[[carries_dependency]]` |

### 8.2 GCC/Clang Type Attributes

| Attribute | Syntax |
|-----------|--------|
| packed | `__attribute__((packed))` |
| aligned | `__attribute__((aligned(N)))` |
| transparent_union | `__attribute__((transparent_union))` |
| may_alias | `__attribute__((may_alias))` |
| designated_init | `__attribute__((designated_init))` |

### 8.3 GCC/Clang Variable Attributes

| Attribute | Syntax |
|-----------|--------|
| aligned | `__attribute__((aligned(N)))` |
| unused | `__attribute__((unused))` |
| used | `__attribute__((used))` |
| section | `__attribute__((section("name")))` |
| weak | `__attribute__((weak))` |
| alias | `__attribute__((alias("target")))` |
| cleanup | `__attribute__((cleanup(func)))` |
| common | `__attribute__((common))` |
| nocommon | `__attribute__((nocommon))` |

### 8.4 GCC/Clang Function Attributes

| Attribute | Syntax |
|-----------|--------|
| noreturn | `__attribute__((noreturn))` |
| noinline | `__attribute__((noinline))` |
| always_inline | `__attribute__((always_inline))` |
| flatten | `__attribute__((flatten))` |
| pure | `__attribute__((pure))` |
| const | `__attribute__((const))` |
| unused | `__attribute__((unused))` |
| used | `__attribute__((used))` |
| weak | `__attribute__((weak))` |
| alias | `__attribute__((alias("target")))` |
| visibility | `__attribute__((visibility("default"\|"hidden"\|"protected")))` |
| constructor | `__attribute__((constructor))` |
| destructor | `__attribute__((destructor))` |
| format | `__attribute__((format(printf, N, M)))` |
| format_arg | `__attribute__((format_arg(N)))` |
| malloc | `__attribute__((malloc))` |
| warn_unused_result | `__attribute__((warn_unused_result))` |
| deprecated | `__attribute__((deprecated))` |
| section | `__attribute__((section("name")))` |
| cold | `__attribute__((cold))` |
| hot | `__attribute__((hot))` |
| naked | `__attribute__((naked))` |
| target | `__attribute__((target("...")))` |
| no_sanitize | `__attribute__((no_sanitize("...")))` |
| interrupt | `__attribute__((interrupt))` |

### 8.5 Combined Attributes

```c
__attribute__((packed, aligned(8)))
```

---

## 9. Preprocessor

### 9.1 Directives

| Directive | Syntax |
|-----------|--------|
| Include (local) | `#include "file"` |
| Include (system) | `#include <file>` |
| Define (object-like) | `#define NAME value` |
| Define (function-like) | `#define FUNC(args) body` |
| Define (variadic) | `#define FUNC(...) body` |
| Undef | `#undef NAME` |
| Ifdef | `#ifdef NAME` |
| Ifndef | `#ifndef NAME` |
| If | `#if expression` |
| Elif | `#elif expression` |
| Else | `#else` |
| Endif | `#endif` |
| Error | `#error "message"` |
| Warning | `#warning "message"` |
| Line | `#line number "filename"` |
| Pragma | `#pragma directive` |

### 9.2 Operators

| Operator | Description |
|----------|-------------|
| `#` | Stringification |
| `##` | Token pasting |
| `defined(NAME)` | Test if macro defined |
| `__has_include(<header>)` | Test if header exists |
| `__has_include("header")` | Test if header exists |
| `__has_attribute(attr)` | Test if attribute supported |
| `__has_builtin(builtin)` | Test if builtin supported |
| `__has_feature(feature)` | Test if feature supported |

### 9.3 Variadic Macro Support

| Macro | Description |
|-------|-------------|
| `__VA_ARGS__` | Variadic arguments |
| `__VA_OPT__(x)` | Optional expansion (C++20) |
| `##__VA_ARGS__` | GNU extension for empty args |

### 9.4 Predefined Macros

| Macro | Description |
|-------|-------------|
| `__FILE__` | Current filename |
| `__LINE__` | Current line number |
| `__func__` | Current function name |
| `__FUNCTION__` | Current function name (GCC) |
| `__PRETTY_FUNCTION__` | Decorated function name (GCC) |
| `__DATE__` | Compilation date |
| `__TIME__` | Compilation time |
| `__COUNTER__` | Unique counter |
| `__cplusplus` | C++ standard version |
| `__STDC_VERSION__` | C standard version |
| `__STDC__` | C conformance |
| `__STDC_HOSTED__` | Hosted/freestanding |

### 9.5 Compiler Detection Macros

| Macro | Compiler |
|-------|----------|
| `__clang__` | Clang |
| `__clang_major__` | Clang major version |
| `__clang_minor__` | Clang minor version |
| `__GNUC__` | GCC major version |
| `__GNUC_MINOR__` | GCC minor version |
| `__GNUC_PATCHLEVEL__` | GCC patch level |
| `_MSC_VER` | MSVC version |

### 9.6 Platform Detection Macros

| Macro | Platform |
|-------|----------|
| `__ANDROID__` | Android |
| `__APPLE__` | macOS/iOS |
| `__FreeBSD__` | FreeBSD |
| `__linux__` | Linux |
| `__MACH__` | macOS |
| `__unix__` | Unix-like |
| `_WIN32` | Windows (32 or 64-bit) |
| `_WIN64` | Windows 64-bit |

### 9.7 Architecture Detection Macros

| Macro | Architecture |
|-------|--------------|
| `__aarch64__` | ARM64 (GCC/Clang) |
| `__arm__` | ARM 32-bit (GCC/Clang) |
| `__i386__` | x86 32-bit (GCC/Clang) |
| `__mips__` | MIPS |
| `__powerpc__` | PowerPC |
| `__riscv` | RISC-V |
| `__x86_64__` | x86-64 (GCC/Clang) |
| `_M_ARM` | ARM 32-bit (MSVC) |
| `_M_ARM64` | ARM64 (MSVC) |
| `_M_IX86` | x86 32-bit (MSVC) |
| `_M_X64` | x86-64 (MSVC) |

### 9.8 Pragma Directives

| Pragma | Description |
|--------|-------------|
| `#pragma once` | Include guard |
| `#pragma pack(push, N)` | Set packing alignment |
| `#pragma pack(pop)` | Restore packing |
| `#pragma message("msg")` | Compiler message |
| `#pragma GCC diagnostic push` | Save diagnostic state |
| `#pragma GCC diagnostic pop` | Restore diagnostic state |
| `#pragma GCC diagnostic ignored "-Wwarning"` | Disable warning |
| `#pragma clang diagnostic ...` | Clang diagnostics |

---

## 10A. Semantic Specifications

### 10A.1 Integer Overflow Behavior

| Context | Behavior | Flag |
|---------|----------|------|
| Signed overflow | Undefined behavior | Default |
| Signed overflow | Wraps (2's complement) | `-fwrapv` |
| Unsigned overflow | Wraps (modulo 2^n) | Always |
| Shift past width | Undefined behavior | Default |

### 10A.2 Null Pointer Behavior

| Operation | Behavior |
|-----------|----------|
| Dereference null | Undefined behavior |
| Compare to null | Well-defined |
| Null in boolean context | `false` |

### 10A.3 Aliasing Rules

| Type | Can alias with |
|------|---------------|
| `char*` | Any type (special case) |
| `unsigned char*` | Any type (special case) |
| `T*` | `T`, cv-qualified `T`, compatible types |
| Strict aliasing | Enabled by default, `-fno-strict-aliasing` disables |

---

## 11. AArch64 Architecture Support

### 11.1 System Register Instructions

| Instruction | Syntax |
|-------------|--------|
| Read system register | `mrs Xd, <sysreg>` |
| Write system register | `msr <sysreg>, Xn` |
| Set DAIF bits | `msr daifset, #imm` |
| Clear DAIF bits | `msr daifclr, #imm` |

### 11.2 System Registers

| Register | Description |
|----------|-------------|
| `DAIF` | Interrupt mask flags |
| `SCTLR_EL1` | System control register |
| `CPACR_EL1` | Coprocessor access control |
| `TCR_EL1` | Translation control register |
| `TTBR0_EL1` | Translation table base 0 |
| `TTBR1_EL1` | Translation table base 1 |
| `MAIR_EL1` | Memory attribute indirection |
| `VBAR_EL1` | Vector base address |
| `ELR_EL1` | Exception link register |
| `SPSR_EL1` | Saved program status |
| `ESR_EL1` | Exception syndrome register |
| `FAR_EL1` | Fault address register |
| `SP_EL0` | User stack pointer |
| `MPIDR_EL1` | Multiprocessor affinity |
| `CNTFRQ_EL0` | Counter frequency |
| `CNTPCT_EL0` | Physical counter |
| `CNTP_CVAL_EL0` | Timer compare value |
| `CNTP_CTL_EL0` | Timer control |
| `FPCR` | Floating-point control |
| `FPSR` | Floating-point status |
| `ICC_*` | GIC CPU interface registers |

### 11.3 Atomic Instructions

| Instruction | Description |
|-------------|-------------|
| `ldaxr Wd, [Xn]` | Load-acquire exclusive (32-bit) |
| `ldaxr Xd, [Xn]` | Load-acquire exclusive (64-bit) |
| `ldar Wd, [Xn]` | Load-acquire (32-bit) |
| `ldar Xd, [Xn]` | Load-acquire (64-bit) |
| `stxr Ws, Wt, [Xn]` | Store-exclusive (32-bit) |
| `stxr Ws, Xt, [Xn]` | Store-exclusive (64-bit) |
| `stlr Wt, [Xn]` | Store-release (32-bit) |
| `stlr Xt, [Xn]` | Store-release (64-bit) |
| `ldadd Ws, Wt, [Xn]` | Atomic add (LSE) |
| `stadd Ws, [Xn]` | Atomic store add (LSE) |
| `swp Ws, Wt, [Xn]` | Atomic swap (LSE) |
| `cas Ws, Wt, [Xn]` | Compare and swap (LSE) |

### 11.4 Memory Barriers

| Instruction | Description |
|-------------|-------------|
| `dmb sy` | Data memory barrier (system) |
| `dmb ish` | Data memory barrier (inner shareable) |
| `dmb ishld` | DMB inner shareable, load |
| `dmb ishst` | DMB inner shareable, store |
| `dsb sy` | Data synchronization barrier |
| `dsb ish` | DSB inner shareable |
| `isb` | Instruction synchronization barrier |

### 11.5 Cache Operations

| Instruction | Description |
|-------------|-------------|
| `dc cvau, Xt` | Data cache clean by VA to PoU |
| `dc civac, Xt` | Data cache clean and invalidate by VA |
| `dc cvac, Xt` | Data cache clean by VA to PoC |
| `dc ivac, Xt` | Data cache invalidate by VA |
| `ic ivau, Xt` | Instruction cache invalidate by VA to PoU |
| `ic iallu` | Instruction cache invalidate all |

### 11.6 TLB Operations

| Instruction | Description |
|-------------|-------------|
| `tlbi vmalle1is` | Invalidate all EL1 TLB entries (inner shareable) |
| `tlbi vale1is, Xt` | Invalidate by VA, last level (inner shareable) |
| `tlbi aside1is, Xt` | Invalidate by ASID (inner shareable) |
| `tlbi vaae1is, Xt` | Invalidate by VA, all ASIDs (inner shareable) |

### 11.7 Exception Instructions

| Instruction | Description |
|-------------|-------------|
| `svc #imm` | Supervisor call |
| `hvc #imm` | Hypervisor call |
| `smc #imm` | Secure monitor call |
| `eret` | Exception return |
| `udf #imm` | Undefined instruction |

### 11.8 Hint Instructions

| Instruction | Description |
|-------------|-------------|
| `nop` | No operation |
| `yield` | Yield to other threads |
| `wfi` | Wait for interrupt |
| `wfe` | Wait for event |
| `sev` | Send event |
| `sevl` | Send event local |

### 11.9 Assembly Directives

| Directive | Description |
|-----------|-------------|
| `.section <name>` | Define section |
| `.text` | Code section |
| `.data` | Initialized data |
| `.bss` | Uninitialized data |
| `.rodata` | Read-only data |
| `.align N` | Align to 2^N bytes |
| `.balign N` | Align to N bytes |
| `.global <name>` | Export symbol |
| `.local <name>` | Local symbol |
| `.type <name>, @function` | Symbol type |
| `.size <name>, size` | Symbol size |
| `.equ <name>, value` | Define constant |
| `.set <name>, value` | Define symbol |
| `.byte value` | Emit byte |
| `.hword value` | Emit halfword |
| `.word value` | Emit word |
| `.quad value` | Emit quadword |
| `.ascii "string"` | Emit string |
| `.asciz "string"` | Emit null-terminated string |
| `.space N` | Reserve N bytes |
| `.skip N` | Skip N bytes |
| `.macro name` | Define macro |
| `.endm` | End macro |

---

## 12. Platform and ABI

**Target: AArch64 Only**

### 12.1 Calling Convention (AAPCS64)

The compiler implements the ARM Architecture Procedure Call Standard for 64-bit.

| Category | Registers | Description |
|----------|-----------|-------------|
| Integer arguments | X0-X7 | First 8 integer/pointer arguments |
| FP/SIMD arguments | V0-V7 | First 8 floating-point/vector arguments |
| Return (integer) | X0, X1 | Integer return values |
| Return (FP/SIMD) | V0-V3 | Floating-point return values |
| Callee-saved | X19-X28 | Must preserve across calls |
| Frame pointer | X29 | Optional frame pointer |
| Link register | X30 | Return address |
| Stack pointer | SP | Must be 16-byte aligned |
| Platform register | X18 | Reserved (do not use) |
| Scratch registers | X9-X15 | Caller-saved temporaries |
| IP registers | X16, X17 | Intra-procedure call (linker veneers) |

**Key Rules:**
- Stack must be 16-byte aligned at public interfaces
- No red zone (SP cannot be used below current allocation)
- Composite types ≤16 bytes passed in registers (HFA/HVA rules apply)
- Larger aggregates passed by reference

### 12.2 Data Type Sizes (AArch64 LP64)

| Type | Size | Alignment | Notes |
|------|------|-----------|-------|
| `char` | 1 | 1 | Unsigned by default on AArch64 |
| `signed char` | 1 | 1 | |
| `unsigned char` | 1 | 1 | |
| `short` | 2 | 2 | |
| `int` | 4 | 4 | |
| `long` | 8 | 8 | LP64 model |
| `long long` | 8 | 8 | |
| `float` | 4 | 4 | IEEE 754 binary32 |
| `double` | 8 | 8 | IEEE 754 binary64 |
| `long double` | 16 | 16 | IEEE 754 binary128 |
| `_Bool` / `bool` | 1 | 1 | |
| `void*` | 8 | 8 | |
| `size_t` | 8 | 8 | |
| `ptrdiff_t` | 8 | 8 | |
| `wchar_t` | 4 | 4 | Signed |

### 12.3 Object File Format

| Target | Format |
|--------|--------|
| Zanna OS | ELF64 (little-endian) |

### 12.4 Name Mangling

| Language | Scheme |
|----------|--------|
| C | No mangling |
| C++ | Itanium C++ ABI |

### 12.5 Itanium C++ ABI Name Mangling

All C++ symbols are mangled according to the Itanium C++ ABI specification.

#### 12.5.1 Mangling Prefix

| Prefix | Meaning |
|--------|---------|
| `_Z` | All C++ mangled names start with `_Z` |
| `_ZN` | Nested name (namespace, class) |
| `_ZL` | Local static |
| `_ZTVN` | Virtual table |
| `_ZTIN` | Type info |
| `_ZTSN` | Type info name |
| `_ZThn` | Non-virtual thunk |
| `_ZTv` | Virtual thunk |
| `_ZGV` | Guard variable |

#### 12.5.2 Type Encoding

| Type | Encoding | Example |
|------|----------|---------|
| `void` | `v` | |
| `bool` | `b` | |
| `char` | `c` | |
| `signed char` | `a` | |
| `unsigned char` | `h` | |
| `short` | `s` | |
| `unsigned short` | `t` | |
| `int` | `i` | |
| `unsigned int` | `j` | |
| `long` | `l` | |
| `unsigned long` | `m` | |
| `long long` | `x` | |
| `unsigned long long` | `y` | |
| `float` | `f` | |
| `double` | `d` | |
| `long double` | `e` | |
| `pointer to T` | `P<T>` | `Pi` = `int*` |
| `reference to T` | `R<T>` | `Ri` = `int&` |
| `rvalue ref to T` | `O<T>` | `Oi` = `int&&` |
| `const T` | `K<T>` | `Ki` = `const int` |
| `volatile T` | `V<T>` | `Vi` = `volatile int` |

#### 12.5.3 Name Length Encoding

Names are encoded as `<length><name>`:
- `3foo` = `foo`
- `10MyFunction` = `MyFunction`
- `12MyNamespace` = `MyNamespace`

#### 12.5.4 Examples

| C++ Declaration | Mangled Name |
|-----------------|--------------|
| `void foo()` | `_Z3foov` |
| `int bar(int x)` | `_Z3bari` |
| `void baz(int*, double)` | `_Z3bazPid` |
| `namespace N { void f(); }` | `_ZN1N1fEv` |
| `class C { void m(); }` | `_ZN1C1mEv` |
| `class C { static int x; }` | `_ZN1C1xE` |
| `C::C()` | `_ZN1CC1Ev` (complete) / `_ZN1CC2Ev` (base) |
| `C::~C()` | `_ZN1CD1Ev` (complete) / `_ZN1CD2Ev` (base) |
| `template<class T> void f(T)` with `f<int>` | `_Z1fIiEvT_` |

### 12.6 Object Layout

#### 12.6.1 Non-Virtual Class Layout

```cpp
class Simple {
    int a;      // offset 0, size 4
    double b;   // offset 8, size 8 (aligned)
    char c;     // offset 16, size 1
    // padding: 7 bytes
};              // total size: 24, alignment: 8
```

Rules:
- Members laid out in declaration order
- Each member aligned to its natural alignment
- Class alignment = max member alignment
- Tail padding added to make size multiple of alignment

#### 12.6.2 Class with Virtual Functions

```cpp
class Virtual {
    // vptr:    offset 0, size 8 (pointer to vtable)
    int a;      // offset 8, size 4
    // padding: 4 bytes
};              // total size: 16, alignment: 8
```

Rules:
- vptr is first member (before any declared members)
- vptr points to vtable for the most-derived class

#### 12.6.3 Single Inheritance Layout

```cpp
class Base {
    // vptr:    offset 0
    int x;      // offset 8
};

class Derived : public Base {
    // Base subobject: offset 0-15
    int y;      // offset 16
};
```

Rules:
- Base subobject placed at beginning of derived
- Base vptr is reused (points to derived vtable)
- Derived members follow base subobject

#### 12.6.4 Multiple Inheritance Layout

```cpp
class A { virtual void f(); int a; };
class B { virtual void g(); int b; };
class C : public A, public B { int c; };
```

Layout of C:
```text
Offset  Member
------  ------
0       A::vptr (points to C's vtable for A)
8       A::a
12      (padding)
16      B::vptr (points to C's vtable for B)
24      B::b
28      C::c
32      (total size)
```

#### 12.6.5 Virtual Inheritance Layout

```cpp
class V { int v; };
class A : virtual public V { int a; };
class B : virtual public V { int b; };
class C : public A, public B { int c; };
```

- Virtual base placed at end of object
- Accessed via vbase offset in vtable
- Only one copy of V in complete C object

### 12.7 Vtable Layout

#### 12.7.1 Vtable Structure (Itanium C++ ABI)

```text
Offset    Contents
------    --------
-24       Virtual base offset (if any)
-16       Offset to top (for multiple inheritance)
-8        RTTI pointer (typeinfo*)
0         Virtual function 0
8         Virtual function 1
16        Virtual function 2
...       ...
```

#### 12.7.2 Simple Virtual Class

```cpp
class Base {
    virtual void f();
    virtual void g();
    virtual ~Base();
};
```

Vtable for Base:
```text
Offset  Content
------  -------
-16     0 (offset to top)
-8      &typeinfo for Base
0       &Base::f
8       &Base::g
16      &Base::~Base() [complete destructor]
24      &Base::~Base() [deleting destructor]
```

#### 12.7.3 Derived Class Vtable

```cpp
class Derived : public Base {
    void f() override;  // overrides Base::f
    virtual void h();   // new virtual
};
```

Vtable for Derived:
```text
Offset  Content
------  -------
-16     0 (offset to top)
-8      &typeinfo for Derived
0       &Derived::f          // overrides Base::f
8       &Base::g             // inherited
16      &Derived::~Derived() [complete]
24      &Derived::~Derived() [deleting]
32      &Derived::h          // new virtual function
```

#### 12.7.4 Thunks

When a function is overridden in a class with multiple inheritance, thunks are generated to adjust `this` pointer:

```cpp
class A { virtual void f(); };
class B { virtual void g(); };
class C : public A, public B {
    void f() override;
    void g() override;  // needs thunk for B's vtable
};
```

Thunk for `C::g()` in B's vtable:
```asm
_ZThn16_N1C1gEv:       // thunk at offset -16
    sub x0, x0, #16    // adjust this pointer
    b _ZN1C1gEv        // jump to actual function
```

### 12.8 RTTI Layout

#### 12.8.1 Type Info Structure

```cpp
struct type_info {
    void* vtable;           // points to type_info vtable
    const char* name;       // mangled name string
};
```

#### 12.8.2 Class Type Info

For polymorphic classes:
```cpp
struct __class_type_info : type_info {
    // no additional members for classes without bases
};

struct __si_class_type_info : type_info {
    const type_info* base;  // single non-virtual base
};

struct __vmi_class_type_info : type_info {
    unsigned int flags;
    unsigned int base_count;
    struct base_info {
        const type_info* base;
        long offset_flags;  // offset and flags
    } bases[];
};
```

## 13. Compiler Flags

### 13.1 Language Standard

| Flag | Description |
|------|-------------|
| `-std=c++20` | C++20 standard |
| `-std=c++17` | C++17 standard |
| `-std=c++14` | C++14 standard |
| `-std=c++11` | C++11 standard |
| `-std=c17` | C17 standard |
| `-std=c11` | C11 standard |
| `-std=gnu++20` | C++20 with GNU extensions |

### 13.2 Warnings

| Flag | Description |
|------|-------------|
| `-Wall` | Enable common warnings |
| `-Wextra` | Enable extra warnings |
| `-Wpedantic` | Strict ISO compliance warnings |
| `-Werror` | Treat warnings as errors |
| `-Werror=<warning>` | Specific warning as error |
| `-Wno-<warning>` | Disable specific warning |

### 13.3 Optimization

| Flag | Description |
|------|-------------|
| `-O0` | No optimization |
| `-O1` | Basic optimization |
| `-O2` | Standard optimization |
| `-O3` | Aggressive optimization |
| `-Os` | Optimize for size |
| `-Oz` | Aggressive size optimization |
| `-Ofast` | Fast math optimizations |
| `-flto` | Link-time optimization |

### 13.4 Debug

| Flag | Description |
|------|-------------|
| `-g` | Debug symbols |
| `-g0` | No debug info |
| `-g1` | Minimal debug info |
| `-g2` | Default debug info |
| `-g3` | Maximum debug info |
| `-ggdb` | GDB-specific debug info |
| `-gdwarf-4` | DWARF version 4 |

### 13.5 Code Generation

| Flag | Description |
|------|-------------|
| `-fPIC` | Position-independent code |
| `-fPIE` | Position-independent executable |
| `-fno-pic` | Disable PIC |
| `-fomit-frame-pointer` | Omit frame pointer |
| `-fno-omit-frame-pointer` | Keep frame pointer |
| `-fstack-protector` | Stack buffer overflow protection |
| `-fno-stack-protector` | Disable stack protector |
| `-fno-exceptions` | Disable C++ exceptions |
| `-fno-rtti` | Disable RTTI |
| `-fno-unwind-tables` | No unwind tables |
| `-fno-asynchronous-unwind-tables` | No async unwind tables |

### 13.6 Freestanding Mode

| Flag | Description |
|------|-------------|
| `-ffreestanding` | Freestanding environment |
| `-fno-builtin` | Disable builtin functions |
| `-nostdlib` | No standard library |
| `-nostdinc` | No standard includes |
| `-nostdinc++` | No C++ standard includes |
| `-nodefaultlibs` | No default libraries |
| `-fno-threadsafe-statics` | No thread-safe static init |
| `-fno-use-cxa-atexit` | No __cxa_atexit |

### 13.7 Target Architecture

| Flag | Description |
|------|-------------|
| `-march=<arch>` | Target architecture |
| `-mtune=<cpu>` | Tune for CPU |
| `-mcpu=<cpu>` | Target CPU |
| `-m32` | 32-bit mode |
| `-m64` | 64-bit mode |
| `--target=<triple>` | Target triple (Clang) |

### 13.8 AArch64-Specific

| Flag | Description |
|------|-------------|
| `-mcpu=cortex-a72` | Target Cortex-A72 |
| `-mstrict-align` | Strict alignment |
| `-mgeneral-regs-only` | No FPU/SIMD |
| `-mno-red-zone` | Disable red zone |
| `-moutline-atomics` | Outline atomics |

### 13.9 Reserved

*Section removed - vcpp targets AArch64 only. See Section 13.8 for target-specific flags.*

### 13.10 Sanitizers

| Flag | Description |
|------|-------------|
| `-fsanitize=address` | AddressSanitizer |
| `-fsanitize=undefined` | UndefinedBehaviorSanitizer |
| `-fsanitize=thread` | ThreadSanitizer |
| `-fsanitize=memory` | MemorySanitizer |
| `-fsanitize=leak` | LeakSanitizer |

### 13.11 Preprocessor

| Flag | Description |
|------|-------------|
| `-D<name>` | Define macro |
| `-D<name>=<value>` | Define macro with value |
| `-U<name>` | Undefine macro |
| `-I<path>` | Add include path |
| `-isystem <path>` | Add system include path |
| `-include <file>` | Force include file |
| `-E` | Preprocess only |

### 13.12 Output

| Flag | Description |
|------|-------------|
| `-c` | Compile only (no link) |
| `-S` | Generate assembly |
| `-o <file>` | Output file |
| `-emit-llvm` | Emit LLVM IR (Clang) |
| `-save-temps` | Save intermediate files |

---

## 14. Linker Requirements

### 14.1 Linking Modes

| Mode | Description |
|------|-------------|
| Static linking | Link all libraries statically |
| Dynamic linking | Link shared libraries |
| Partial linking | Combine object files |

### 14.2 Linker Flags

| Flag | Description |
|------|-------------|
| `-l<lib>` | Link library |
| `-L<path>` | Add library search path |
| `-static` | Static linking |
| `-shared` | Create shared library |
| `-pie` | Position-independent executable |
| `-no-pie` | Non-PIE executable |
| `-T <script>` | Use linker script |
| `-e <symbol>` | Entry point |
| `--gc-sections` | Remove unused sections |
| `--no-undefined` | Error on undefined symbols |
| `-z noexecstack` | Non-executable stack |
| `-z relro` | Read-only relocations |
| `-z now` | Immediate binding |

### 14.3 Output Formats

| Format | Description |
|--------|-------------|
| ELF64 | Target object/executable format (AArch64) |
| Static library (.a) | Archive of objects |

**Note:** vcpp only generates ELF64 for the AArch64 target (Zanna OS). The host compiler (building vcpp itself) uses native formats (Mach-O on macOS, ELF on Linux).

### 14.4 Debug Information

| Format | Description |
|--------|-------------|
| DWARF | Debug With Arbitrary Record Formats |
| DWARF 4 | Common version |
| DWARF 5 | Latest version |

---

*Zanna C++ Compiler Specifications v1.0*
