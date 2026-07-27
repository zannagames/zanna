---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Zanna C++ Compiler Requirements Specification

This document specifies the complete set of C and C++ language features, standard library
components, compiler extensions, and runtime requirements necessary to build the entire
Zanna project. It serves as the definitive specification for the Zanna C++ system compiler.

---

## Table of Contents

1. [Language Standard Requirements](#1-language-standard-requirements)
2. [C++ Core Language Features](#2-c-core-language-features)
3. [C Core Language Features](#3-c-core-language-features)
4. [C++ Standard Library Requirements](#4-c-standard-library-requirements)
5. [C Standard Library Requirements](#5-c-standard-library-requirements)
6. [Compiler Builtins and Extensions](#6-compiler-builtins-and-extensions)
7. [Preprocessor Requirements](#7-preprocessor-requirements)
8. [Platform and ABI Requirements](#8-platform-and-abi-requirements)
9. [Freestanding Mode Requirements](#9-freestanding-mode-requirements)
10. [Compilation Modes and Flags](#10-compilation-modes-and-flags)
11. [Zanna OS Kernel and Userland Requirements](#11-zanna-os-kernel-and-userland-requirements)

---

## 1. Language Standard Requirements

### Target Standards

| Standard | Version | Rationale |
|----------|---------|-----------|
| C++ | **C++20** | Required for Zanna OS kernel and userspace compilation |
| C | **C17** | Required for libc, bootloader, and C source files |

### Host and Target Platforms

| Platform Type | Supported | Notes |
|---------------|-----------|-------|
| **Host (where vcpp runs)** | macOS (ARM64), Linux (x86-64, ARM64) | Development environments |
| **Target (code vcpp generates)** | **AArch64 only** | Zanna OS target |

| Target Property | Value |
|-----------------|-------|
| Architecture | AArch64 (ARM64) |
| CPU Baseline | Cortex-A72 |
| Endianness | Little-endian |
| Other target architectures | Not supported |

### Component Standard Matrix

| Component | Language | Standard | Exception Handling | RTTI |
|-----------|----------|----------|-------------------|------|
| Zanna Compiler (vcpp) | C++ | C++20 | Enabled | Enabled |
| Zanna OS Kernel | C++ | C++20 | **Disabled** | **Disabled** |
| Zanna OS Userspace | C++ | C++20 | **Disabled** | **Disabled** |
| Zanna libc | C | C17 | N/A | N/A |
| Zanna Bootloader | C | C17 | N/A | N/A |

### Exception Handling Policy

- **Kernel/Userspace**: Compiled with `-fno-exceptions`. Code must not use `throw`, `try`, or `catch`.
- **Compiler itself**: May use exceptions internally (compiles with host compiler).
- **Required runtime support**: None for target. The compiler does not generate exception handling code when `-fno-exceptions` is specified.

### C++20 Features Actually Used

Based on analysis of Zanna OS source code, the following C++20 features are used:
- Designated initializers (C++20 for aggregates with bases)
- `[[nodiscard]]`, `[[maybe_unused]]` attributes
- Nested inline namespaces

The following C++20 features are **not used** and have lower implementation priority:
- Concepts and requires clauses
- Coroutines
- Modules
- `consteval`, `constinit`
- Three-way comparison (`<=>`)

### Critical Extension Requirements

These compiler extensions are **mandatory** for compiling Zanna OS. Without them, the kernel and libc cannot be built.

| Feature | Priority | Usage | Files |
|---------|----------|-------|-------|
| `__attribute__((packed))` | **CRITICAL** | ZannaFS structures, device drivers | `virtio.hpp`, `ext2.hpp` |
| `__attribute__((aligned(N)))` | **CRITICAL** | Page tables, DMA buffers | `mmu.cpp`, `virtio.cpp` |
| `__attribute__((section("...")))` | **CRITICAL** | Linker section placement | `crt0.c`, `kernel.ld` |
| `__attribute__((noreturn))` | High | `panic()`, `_exit()` | `panic.cpp`, `exit.c` |
| `__attribute__((used))` | High | Prevent dead code elimination | ISR handlers |
| `__attribute__((naked))` | High | Exception vectors | `exceptions.S` |
| `__builtin_offsetof(type, member)` | High | VirtIO drivers, struct introspection | `virtio.cpp` |

### ABI Compatibility Requirements

| Requirement | Specification | Notes |
|-------------|---------------|-------|
| Vtable layout | Itanium C++ ABI | For virtual function calls |
| Name mangling | Itanium C++ ABI | For linking with libstdc++/libc++ |
| Exception handling | Not applicable | Compiled with `-fno-exceptions` |
| RTTI layout | Itanium C++ ABI | When RTTI enabled (userspace optional) |
| Thread-local storage | AAPCS64 TLS ABI | `thread_local` variables in userspace |

---

## 2. C++ Core Language Features

### 2.1 Classes and Object-Oriented Programming

| Feature | Example | Files Using |
|---------|---------|-------------|
| Class definitions | `class Module { ... };` | All `.hpp` files |
| Public/private/protected access | Member visibility | All classes |
| Constructors/destructors | `Module(); ~Module();` | All classes |
| Copy/move constructors | `Module(Module&&)` | `Module.cpp`, `Function.cpp` |
| Copy/move assignment | `operator=(Module&&)` | `Module.cpp`, `Value.hpp` |
| `= default` specifier | `~Module() = default;` | Many classes |
| `= delete` specifier | `operator=(const X&) = delete;` | `VMContext.hpp` |
| Member initializer lists | `: member_(value)` | All constructors |
| In-class member initializers | `int x = 0;` | `VMContext.hpp`, `SimplifyCFG.hpp` |
| `explicit` constructors | `explicit Module(StringRef)` | Many classes |
| Virtual functions | `virtual void run()` | Transform passes |
| Pure virtual functions | `= 0` | Abstract interfaces |
| Inheritance (single/multiple) | `: public Base` | Pass framework |
| `final` specifier | `class Derived final` | Various |
| `override` specifier | `void run() override` | Virtual overrides |

### 2.2 Templates

| Feature | Example | Files Using |
|---------|---------|-------------|
| Class templates | `template<typename T> class X` | `function_ref.hpp`, containers |
| Function templates | `template<typename T> T func()` | Many utilities |
| Full template specialization | `template<> class X<int>` | Type traits |
| Partial template specialization | `template<typename T> class X<T*>` | Type traits |
| Variadic templates | `template<typename... Args>` | `function_ref.hpp`, tuples |
| Parameter pack expansion | `Args...` | Variadic functions |
| Template alias (`using`) | `using type = T;` | Type traits |
| Non-type template parameters | `template<size_t N>` | `std::array`, constants |
| Template argument deduction | Class template argument deduction | C++17 |
| SFINAE | `std::enable_if_t<...>` | `function_ref.hpp` |

### 2.3 Modern C++11 Features

| Feature | Example | Files Using |
|---------|---------|-------------|
| `auto` type deduction | `auto x = func();` | Throughout codebase |
| `decltype` | `decltype(expr)` | Type inference |
| Range-based for loops | `for (auto& x : container)` | Throughout codebase |
| Initializer lists | `{1, 2, 3}` | Container initialization |
| Uniform initialization | `Type{args}` | Object construction |
| Lambda expressions | `[&](int x) { return x; }` | Algorithms, callbacks |
| Lambda captures | `[=]`, `[&]`, `[this]`, `[x=move(y)]` | Many files |
| Move semantics | `std::move(x)` | Smart pointers, containers |
| Rvalue references | `T&&` | Move constructors |
| Perfect forwarding | `std::forward<T>(x)` | Template utilities |
| `nullptr` | `nullptr` instead of `NULL` | Throughout |
| Scoped enums | `enum class Opcode : uint8_t` | `Instr.hpp`, many enums |
| `noexcept` specifier | `void func() noexcept` | `VMContext.hpp`, many |
| `constexpr` functions | `constexpr int f()` | Constants, compile-time |
| `static_assert` | `static_assert(cond, "msg")` | Type validation |
| `override`/`final` | Virtual function specifiers | OOP hierarchies |
| Default/deleted functions | `= default`, `= delete` | Rule of 5/0 |
| User-defined literals | `operator"" _suffix` | Potential use |
| `thread_local` storage | `thread_local VM* vm;` | VM context, traps |
| Alias templates | `template<class T> using X = Y<T>;` | Type utilities |
| Variadic templates | `template<typename... Ts>` | Tuple, variant |

### 2.4 Modern C++14 Features

| Feature | Example | Files Using |
|---------|---------|-------------|
| Generic lambdas | `[](auto x) { ... }` | Algorithm callbacks |
| Lambda init-captures | `[x = std::move(y)]` | Resource transfer |
| `decltype(auto)` | Return type deduction | Template utilities |
| Variable templates | `template<class T> constexpr T pi` | Constants |
| `[[deprecated]]` attribute | Function deprecation | API evolution |
| Binary literals | `0b1010` | Bit manipulation |
| Digit separators | `1'000'000` | Large numbers |
| `std::make_unique` | `std::make_unique<T>()` | Smart pointer creation |

### 2.5 Modern C++17 Features (Required)

| Feature | Example | Files Using |
|---------|---------|-------------|
| Structured bindings | `auto [a, b] = pair;` | `Lowerer_Stmt.cpp`, TUI |
| `if` with initializer | `if (auto x = f(); x > 0)` | Control flow |
| `std::optional<T>` | `std::optional<Slot>` | `VMContext.hpp`, returns |
| `std::variant<Ts...>` | Discriminated unions | Value types |
| `std::string_view` | Non-owning string ref | `SimplifyCFG.hpp`, I/O |
| `[[nodiscard]]` attribute | Return value warning | Memory allocators |
| `[[maybe_unused]]` attribute | Suppress warnings | Padding, debug |
| `[[noreturn]]` attribute | No-return functions | `exit()`, traps |
| `inline` variables | `inline constexpr int x = 5;` | Header constants |
| Nested namespace definition | `namespace a::b::c { }` | Potential use |
| `constexpr if` | `if constexpr (cond)` | Template metaprogramming |
| Fold expressions | `(args + ...)` | Variadic templates |
| Class template argument deduction | `std::vector v{1,2,3};` | Convenience |
| `std::byte` | Byte manipulation | Binary data |

### 2.6 Namespaces

| Feature | Example | Files Using |
|---------|---------|-------------|
| Namespace declaration | `namespace il::core { }` | All C++ headers |
| Nested namespaces | `namespace il::core::vm { }` | Module organization |
| Anonymous namespaces | `namespace { }` | File-local symbols |
| `using` declarations | `using std::string;` | Convenience |
| `using namespace` | `using namespace std;` | Limited use |
| Inline namespaces | `inline namespace v1 { }` | ABI versioning |

### 2.7 Exception Handling

| Feature | Example | Files Using |
|---------|---------|-------------|
| `try`/`catch`/`throw` | Exception handling | Error paths |
| `noexcept` specifier | `void f() noexcept` | Performance-critical |
| `noexcept(expr)` | Conditional noexcept | Move operations |
| Standard exception types | `std::runtime_error` | Error reporting |

---

## 3. C Core Language Features

### 3.1 C11 Features

| Feature | Example | Files Using |
|---------|---------|-------------|
| Fixed-width integers | `int32_t`, `uint64_t` | All C files |
| `bool` type | `stdbool.h` / `_Bool` | Logic operations |
| Designated initializers | `.field = value` | Struct initialization |
| Compound literals | `(type){values}` | Inline struct creation |
| `inline` functions | `inline int f()` | Performance-critical |
| `restrict` keyword | `void* restrict p` | Aliasing hints |
| `//` comments | Single-line comments | Throughout |
| Variable declarations anywhere | Not just block start | Modern style |
| `snprintf` | Safe string formatting | `stdio.c` |
| `long long` type | 64-bit integers | Large values |
| Variadic macros | `#define LOG(...)` | Debugging |
| `__func__` predefined | Current function name | Debugging |

### 3.2 C11 Features

| Feature | Example | Files Using |
|---------|---------|-------------|
| `_Static_assert` | Compile-time assertions | Structure validation |
| Anonymous structs/unions | Nested without name | Compact layout |
| `_Alignas`/`_Alignof` | Alignment control | Memory layout |
| `_Generic` | Type-generic expressions | Optional |
| `_Noreturn` | Functions that don't return | `abort()`, `exit()` |
| `<stdatomic.h>` concepts | Atomic operations | Concurrency |

### 3.3 Type System

| Category | Types Required |
|----------|----------------|
| Integers | `char`, `short`, `int`, `long`, `long long` (signed/unsigned) |
| Fixed-width | `int8_t`, `int16_t`, `int32_t`, `int64_t`, `uint*_t` variants |
| Size types | `size_t`, `ssize_t`, `ptrdiff_t`, `intptr_t`, `uintptr_t` |
| Floating point | `float`, `double`, `long double` |
| Character | `char`, `unsigned char`, `signed char` |
| Pointer | Pointer types, pointer arithmetic, function pointers |
| Aggregate | `struct`, `union`, arrays |
| Enum | Enumeration types |

### 3.4 Operators and Expressions

| Category | Operators |
|----------|-----------|
| Arithmetic | `+`, `-`, `*`, `/`, `%` |
| Bitwise | `&`, `\|`, `^`, `~`, `<<`, `>>` |
| Comparison | `==`, `!=`, `<`, `>`, `<=`, `>=` |
| Logical | `&&`, `\|\|`, `!` |
| Assignment | `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `\|=`, `^=`, `<<=`, `>>=` |
| Increment/Decrement | `++`, `--` (prefix and postfix) |
| Pointer | `*` (dereference), `&` (address-of), `->`, `.` |
| Ternary | `?:` |
| Comma | `,` |
| Sizeof | `sizeof(type)`, `sizeof expr` |
| Cast | `(type)expr` |

---

## 4. C++ Standard Library Requirements

### 4.1 Containers

| Header | Components | Usage |
|--------|------------|-------|
| `<vector>` | `std::vector<T>` | Dynamic arrays throughout |
| `<array>` | `std::array<T, N>` | Fixed-size arrays |
| `<string>` | `std::string` | String manipulation |
| `<map>` | `std::map<K, V>` | Ordered key-value |
| `<set>` | `std::set<T>` | Ordered unique elements |
| `<unordered_map>` | `std::unordered_map<K, V>` | Hash tables |
| `<unordered_set>` | `std::unordered_set<T>` | Hash sets |
| `<deque>` | `std::deque<T>` | Double-ended queue |
| `<list>` | `std::list<T>` | Linked lists |
| `<forward_list>` | `std::forward_list<T>` | Singly-linked list |
| `<queue>` | `std::queue<T>`, `std::priority_queue<T>` | FIFO queues |
| `<stack>` | `std::stack<T>` | LIFO stack |

### 4.2 Utilities

| Header | Components | Usage |
|--------|------------|-------|
| `<optional>` | `std::optional<T>` | Nullable values |
| `<variant>` | `std::variant<Ts...>`, `std::get`, `std::get_if`, `std::holds_alternative` | Tagged unions |
| `<tuple>` | `std::tuple<Ts...>`, `std::get<I>`, `std::make_tuple` | Heterogeneous collections |
| `<pair>` / `<utility>` | `std::pair<T, U>`, `std::make_pair` | Two-element tuples |
| `<memory>` | `std::unique_ptr<T>`, `std::make_unique`, `std::shared_ptr<T>` | Smart pointers |
| `<functional>` | `std::function<Sig>`, `std::invoke`, `std::reference_wrapper` | Callables |
| `<any>` | `std::any` | Type-erased storage |
| `<bitset>` | `std::bitset<N>` | Fixed-size bit arrays |

### 4.3 Algorithms and Iterators

| Header | Components | Usage |
|--------|------------|-------|
| `<algorithm>` | `std::sort`, `std::find`, `std::copy`, `std::transform`, `std::for_each`, `std::remove_if`, etc. | Container algorithms |
| `<numeric>` | `std::accumulate`, `std::iota`, `std::gcd`, `std::lcm` | Numeric algorithms |
| `<iterator>` | Iterator traits, `std::begin`, `std::end`, `std::back_inserter` | Iterator utilities |

### 4.4 Strings and I/O

| Header | Components | Usage |
|--------|------------|-------|
| `<string>` | `std::string`, `std::to_string` | String handling |
| `<string_view>` | `std::string_view` | Non-owning string refs |
| `<sstream>` | `std::stringstream`, `std::ostringstream` | String streams |
| `<iostream>` | `std::cout`, `std::cerr`, `std::cin` | Standard I/O |
| `<fstream>` | `std::ifstream`, `std::ofstream` | File I/O |
| `<iomanip>` | `std::setw`, `std::setfill`, `std::hex` | I/O formatting |
| `<charconv>` | `std::to_chars`, `std::from_chars` | Fast number conversion |

### 4.5 Type Traits and Metaprogramming

| Header | Components | Usage |
|--------|------------|-------|
| `<type_traits>` | `std::enable_if_t`, `std::is_same_v`, `std::remove_cv_t`, `std::remove_reference_t`, `std::decay_t`, `std::is_integral_v`, `std::is_trivially_copyable_v`, etc. | Template metaprogramming |
| `<limits>` | `std::numeric_limits<T>` | Type limits |

### 4.6 Concurrency

| Header | Components | Usage |
|--------|------------|-------|
| `<thread>` | `std::thread` | Thread creation |
| `<mutex>` | `std::mutex`, `std::lock_guard`, `std::unique_lock` | Synchronization |
| `<condition_variable>` | `std::condition_variable` | Thread signaling |
| `<atomic>` | `std::atomic<T>`, `std::atomic_flag` | Atomic operations |
| `<future>` | `std::future`, `std::promise`, `std::async` | Async operations |

### 4.7 Time and Chrono

| Header | Components | Usage |
|--------|------------|-------|
| `<chrono>` | `std::chrono::duration`, `std::chrono::time_point`, `std::chrono::steady_clock` | Time measurement |
| `<ctime>` | `std::time`, `std::clock` | C-style time |

### 4.8 Error Handling

| Header | Components | Usage |
|--------|------------|-------|
| `<exception>` | `std::exception`, `std::terminate` | Exception base |
| `<stdexcept>` | `std::runtime_error`, `std::logic_error`, `std::out_of_range` | Standard exceptions |
| `<system_error>` | `std::error_code`, `std::system_error` | System errors |

### 4.9 C Compatibility Headers

| Header | C Header | Purpose |
|--------|----------|---------|
| `<cstdint>` | `<stdint.h>` | Fixed-width integers |
| `<cstddef>` | `<stddef.h>` | `size_t`, `nullptr_t`, `offsetof` |
| `<cstdlib>` | `<stdlib.h>` | General utilities |
| `<cstdio>` | `<stdio.h>` | C-style I/O |
| `<cstring>` | `<string.h>` | C string functions |
| `<cmath>` | `<math.h>` | Math functions |
| `<cassert>` | `<assert.h>` | Assertions |
| `<climits>` | `<limits.h>` | Integer limits |
| `<cerrno>` | `<errno.h>` | Error numbers |
| `<cctype>` | `<ctype.h>` | Character classification |

---

## 5. C Standard Library Requirements

### 5.1 Core Headers

| Header | Required Functions |
|--------|-------------------|
| `<stdio.h>` | `printf`, `fprintf`, `sprintf`, `snprintf`, `vprintf`, `vfprintf`, `vsprintf`, `vsnprintf`, `fopen`, `fclose`, `fread`, `fwrite`, `fgets`, `fputs`, `fputc`, `fgetc`, `fseek`, `ftell`, `rewind`, `fflush`, `feof`, `ferror`, `perror`, `sscanf`, `getchar`, `putchar`, `puts`, `remove`, `rename`, `tmpfile`, `tmpnam`, `fileno`, `fdopen`, `freopen` |
| `<stdlib.h>` | `malloc`, `calloc`, `realloc`, `free`, `atoi`, `atol`, `atoll`, `atof`, `strtol`, `strtoul`, `strtoll`, `strtoull`, `strtod`, `strtof`, `strtold`, `abs`, `labs`, `llabs`, `div`, `ldiv`, `lldiv`, `exit`, `_Exit`, `abort`, `atexit`, `getenv`, `setenv`, `unsetenv`, `putenv`, `system`, `qsort`, `bsearch`, `rand`, `srand` |
| `<string.h>` | `strlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr`, `strpbrk`, `strspn`, `strcspn`, `strtok`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strerror` |
| `<stdint.h>` | All fixed-width types (`int8_t` through `int64_t`, `uint8_t` through `uint64_t`), `intptr_t`, `uintptr_t`, `intmax_t`, `uintmax_t`, `INT*_MIN/MAX`, `UINT*_MAX`, `SIZE_MAX`, `INTPTR_MIN/MAX` |
| `<stddef.h>` | `size_t`, `ptrdiff_t`, `NULL`, `offsetof` |
| `<stdbool.h>` | `bool`, `true`, `false` |
| `<stdarg.h>` | `va_list`, `va_start`, `va_arg`, `va_end`, `va_copy` |

### 5.2 Additional Headers

| Header | Required Functions/Macros |
|--------|--------------------------|
| `<limits.h>` | `CHAR_BIT`, `CHAR_MIN/MAX`, `SCHAR_MIN/MAX`, `UCHAR_MAX`, `SHRT_MIN/MAX`, `USHRT_MAX`, `INT_MIN/MAX`, `UINT_MAX`, `LONG_MIN/MAX`, `ULONG_MAX`, `LLONG_MIN/MAX`, `ULLONG_MAX` |
| `<errno.h>` | `errno`, `ENOENT`, `EINVAL`, `ENOMEM`, `EBADF`, `EACCES`, `EEXIST`, `ETIMEDOUT`, `EINTR`, etc. |
| `<assert.h>` | `assert()` macro |
| `<ctype.h>` | `isalpha`, `isdigit`, `isalnum`, `isspace`, `isupper`, `islower`, `isprint`, `ispunct`, `iscntrl`, `isxdigit`, `toupper`, `tolower` |
| `<math.h>` | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `exp`, `log`, `log10`, `log2`, `pow`, `sqrt`, `cbrt`, `hypot`, `ceil`, `floor`, `round`, `trunc`, `fmod`, `remainder`, `fabs`, `fmax`, `fmin`, `NAN`, `INFINITY`, `isnan`, `isinf`, `isfinite` |
| `<time.h>` | `time`, `clock`, `difftime`, `mktime`, `localtime`, `gmtime`, `strftime`, `clock_gettime` (POSIX), `CLOCK_REALTIME`, `CLOCK_MONOTONIC` |
| `<signal.h>` | `signal`, `raise`, `SIGINT`, `SIGTERM`, `SIGSEGV`, `SIGABRT` |
| `<setjmp.h>` | `setjmp`, `longjmp`, `jmp_buf` |
| `<inttypes.h>` | `PRId32`, `PRIu64`, `PRIx64`, `SCNd32`, etc. (format specifiers) |
| `<float.h>` | `FLT_MIN`, `FLT_MAX`, `FLT_EPSILON`, `DBL_MIN`, `DBL_MAX`, `DBL_EPSILON`, etc. |

### 5.3 POSIX Extensions (Zanna libc Implementation Targets)

**Note**: These are functions that the Zanna libc implements. The compiler itself does not need to provide these—it only needs to correctly compile code that calls them.

| Header | Functions Implemented by Zanna libc |
|--------|-------------------------------------|
| `<unistd.h>` | `read`, `write`, `close`, `lseek`, `unlink`, `access`, `getcwd`, `chdir`, `sleep`, `usleep`, `sbrk` |
| `<fcntl.h>` | `open`, `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND` |
| `<sys/types.h>` | `off_t`, `ssize_t`, `mode_t`, `pid_t` |
| `<sys/stat.h>` | `stat`, `fstat`, `mkdir`, `S_ISREG`, `S_ISDIR` |
| `<pthread.h>` | `pthread_create`, `pthread_join`, `pthread_detach`, `pthread_self`, `pthread_equal`, `pthread_mutex_*`, `pthread_cond_*` |
| `<sched.h>` | `sched_yield` |

---

## 6. Compiler Builtins and Extensions

### 6.1 Required GCC/Clang Builtins

| Builtin | Purpose | Usage |
|---------|---------|-------|
| `__builtin_add_overflow(a, b, res)` | Checked addition | Safe arithmetic |
| `__builtin_assume(cond)` | Assumption for optimizer | Optimization |
| `__builtin_bswap16/32/64(x)` | Byte swap | Endian conversion |
| `__builtin_clz(x)` | Count leading zeros | Bit manipulation |
| `__builtin_ctz(x)` | Count trailing zeros | Bit manipulation |
| `__builtin_expect(expr, val)` | Branch prediction | Performance hints |
| `__builtin_huge_val()` | HUGE_VAL | Overflow handling |
| `__builtin_inf()` | Positive infinity | Float special values |
| `__builtin_memcpy(d, s, n)` | Optimized memcpy | Memory operations |
| `__builtin_memmove(d, s, n)` | Optimized memmove | Memory operations |
| `__builtin_memset(s, c, n)` | Optimized memset | Memory operations |
| `__builtin_mul_overflow(a, b, res)` | Checked multiplication | Safe arithmetic |
| `__builtin_nan("")` | Quiet NaN | Float special values |
| `__builtin_popcount(x)` | Population count | Bit counting |
| `__builtin_sub_overflow(a, b, res)` | Checked subtraction | Safe arithmetic |
| `__builtin_unreachable()` | Unreachable code marker | Optimization |
| `__builtin_va_arg(ap, type)` | Get next variadic argument | Variadic functions |
| `__builtin_va_copy(dest, src)` | Copy va_list | Variadic forwarding |
| `__builtin_va_end(ap)` | End variadic processing | Variadic functions |
| `__builtin_va_list` | Variadic argument list type | `stdarg.h` implementation |
| `__builtin_va_start(ap, last)` | Start variadic processing | Variadic functions |

### 6.2 Atomic Builtins (GCC-style)

| Builtin | Purpose |
|---------|---------|
| `__atomic_load_n(ptr, order)` | Atomic load |
| `__atomic_store_n(ptr, val, order)` | Atomic store |
| `__atomic_exchange_n(ptr, val, order)` | Atomic exchange |
| `__atomic_compare_exchange_n(ptr, expected, desired, weak, succ, fail)` | CAS operation |
| `__atomic_fetch_add(ptr, val, order)` | Atomic fetch-and-add |
| `__atomic_fetch_sub(ptr, val, order)` | Atomic fetch-and-subtract |
| `__atomic_fetch_and(ptr, val, order)` | Atomic fetch-and-AND |
| `__atomic_fetch_or(ptr, val, order)` | Atomic fetch-and-OR |
| `__atomic_fetch_xor(ptr, val, order)` | Atomic fetch-and-XOR |

Memory order constants:
- `__ATOMIC_RELAXED`
- `__ATOMIC_CONSUME`
- `__ATOMIC_ACQUIRE`
- `__ATOMIC_RELEASE`
- `__ATOMIC_ACQ_REL`
- `__ATOMIC_SEQ_CST`

### 6.3 Type Attributes

| Attribute | Purpose | Example |
|-----------|---------|---------|
| `__attribute__((aligned(N)))` | Alignment control | Memory layout |
| `__attribute__((always_inline))` | Force inlining | Critical paths |
| `__attribute__((constructor))` | Run before main | Initialization |
| `__attribute__((destructor))` | Run after main | Cleanup |
| `__attribute__((format(printf, N, M)))` | Format string checking | Printf-like |
| `__attribute__((noinline))` | Prevent inlining | Debug support |
| `__attribute__((noreturn))` | No-return function | `abort()`, `exit()` |
| `__attribute__((packed))` | Remove padding | Compact structs |
| `__attribute__((unused))` | Suppress unused warnings | Optional params |
| `__attribute__((visibility("default")))` | Symbol visibility | Shared libraries |
| `__attribute__((weak))` | Weak symbol | Optional implementations |

---

## 7. Preprocessor Requirements

### 7.1 Standard Directives

| Directive | Description |
|-----------|-------------|
| `#include "file"` | Include local header |
| `#include <file>` | Include system header |
| `#define NAME value` | Simple macro |
| `#define FUNC(args)` | Function-like macro |
| `#define VARIADIC(...)` | Variadic macro |
| `#undef NAME` | Undefine macro |
| `#ifdef NAME` | Conditional on definition |
| `#ifndef NAME` | Conditional on non-definition |
| `#if expression` | Conditional on expression |
| `#elif expression` | Else-if branch |
| `#else` | Else branch |
| `#endif` | End conditional |
| `#pragma once` | Include guard (preferred) |
| `#pragma pack(push/pop)` | Packing control |
| `#error "message"` | Compilation error |
| `#warning "message"` | Compilation warning |
| `#line` | Line number override |

### 7.2 Predefined Macros

| Macro | Purpose |
|-------|---------|
| `__FILE__` | Current filename |
| `__LINE__` | Current line number |
| `__func__` / `__FUNCTION__` | Current function name |
| `__DATE__` | Compilation date |
| `__TIME__` | Compilation time |
| `__cplusplus` | C++ standard version |
| `__STDC_VERSION__` | C standard version |
| `__GNUC__` | GCC major version |
| `__clang__` | Clang compiler |
| `NDEBUG` | Release mode |

### 7.3 Platform Detection Macros

| Macro | Platform |
|-------|----------|
| `__ANDROID__` | Android |
| `__APPLE__` | macOS/iOS |
| `__linux__` | Linux |
| `__unix__` | Unix-like systems |
| `_WIN32` | Windows (32 or 64-bit) |
| `_WIN64` | Windows 64-bit |

### 7.4 Architecture Detection

| Macro | Architecture |
|-------|--------------|
| `__aarch64__` / `_M_ARM64` | ARM64 |
| `__arm__` / `_M_ARM` | ARM 32-bit |
| `__i386__` / `_M_IX86` | x86 32-bit |
| `__riscv` | RISC-V |
| `__x86_64__` / `_M_X64` | x86-64 |

---

## 8. Platform and ABI Requirements

**Target Architecture: AArch64 Only**

### 8.1 Calling Convention (AAPCS64)

The compiler implements the ARM 64-bit Architecture Procedure Call Standard.

| Category | Registers | Purpose |
|----------|-----------|---------|
| Arguments (integer) | X0-X7 | First 8 integer/pointer arguments |
| Arguments (FP/SIMD) | V0-V7 | First 8 floating-point arguments |
| Return (integer) | X0, X1 | Integer return values |
| Return (FP/SIMD) | V0-V3 | Floating-point return values |
| Callee-saved | X19-X28 | Must be preserved across calls |
| Frame pointer | X29 | Frame pointer (optional) |
| Link register | X30 | Return address |
| Stack pointer | SP | 16-byte aligned |
| Platform register | X18 | Reserved (do not use) |
| Temporary | X9-X15 | Caller-saved scratch registers |
| Intra-procedure | X16, X17 | Used by linker veneers |

**Key AAPCS64 Rules:**
- Stack must be 16-byte aligned at function calls
- No red zone (unlike x86-64 SysV ABI)
- Composite types ≤16 bytes passed in registers
- Larger composites passed by reference

### 8.2 Data Type Requirements (AArch64 LP64)

| Type | Size | Alignment | Notes |
|------|------|-----------|-------|
| `char` | 1 | 1 | Unsigned by default on AArch64 |
| `signed char` | 1 | 1 | |
| `short` | 2 | 2 | |
| `int` | 4 | 4 | |
| `long` | 8 | 8 | LP64 data model |
| `long long` | 8 | 8 | |
| `float` | 4 | 4 | IEEE 754 binary32 |
| `double` | 8 | 8 | IEEE 754 binary64 |
| `long double` | 16 | 16 | IEEE 754 binary128 (quad precision) |
| `_Bool` / `bool` | 1 | 1 | |
| Pointers | 8 | 8 | |
| `size_t` | 8 | 8 | |
| `ptrdiff_t` | 8 | 8 | |
| `wchar_t` | 4 | 4 | Signed |

### 8.3 Object File Format

| Target | Format |
|--------|--------|
| Zanna OS | ELF64 (little-endian) |
| Linux AArch64 | ELF64 (for cross-development) |

### 8.4 Name Mangling

The compiler uses the Itanium C++ ABI for name mangling (same as GCC and Clang on AArch64).

---

## 9. Freestanding Mode Requirements

The Zanna OS libc operates in freestanding mode with these requirements:

### 9.1 No Standard Library Dependencies

The freestanding environment must NOT require:
- Exception handling runtime (`libcxxabi`, `libgcc_eh`)
- RTTI support
- Global constructors/destructors (unless explicitly used)
- Thread-local storage runtime (must use compiler builtins)
- Dynamic linking infrastructure

### 9.2 Required Freestanding Headers

| Header | Purpose |
|--------|---------|
| `<stddef.h>` | `size_t`, `NULL`, `offsetof` |
| `<stdint.h>` | Fixed-width integers |
| `<stdarg.h>` | Variadic arguments (via builtins) |
| `<stdbool.h>` | Boolean type |
| `<limits.h>` | Integer limits |
| `<float.h>` | Floating-point limits |
| `<stdnoreturn.h>` | `noreturn` macro |
| `<stdalign.h>` | Alignment macros |

### 9.3 Compiler Flags for Freestanding (AArch64)

```text
-ffreestanding
-fno-exceptions
-fno-rtti
-fno-unwind-tables
-fno-asynchronous-unwind-tables
-fno-stack-protector
-fno-threadsafe-statics
-fno-use-cxa-atexit
-nostdlib
-nostdinc (when providing custom headers)
```

---

## 10. Compilation Modes and Flags

### 10.1 Required Compiler Flags

```bash
# C++ Standard
-std=c++20

# C Standard
-std=c17

# Warnings (recommended)
-Wall
-Wextra
-Wpedantic
-Werror=return-type
-Werror=uninitialized

# Debug Mode
-g
-O0
-DDEBUG

# Release Mode
-O2 or -O3
-DNDEBUG

# Position Independent Code (for shared libs)
-fPIC

# Sanitizers (development)
-fsanitize=address
-fsanitize=undefined
-fsanitize=thread
```

### 10.2 Linker Requirements

| Feature | Requirement |
|---------|-------------|
| Static linking | Full static library support |
| Dynamic linking | Shared library support (`.so`, `.dylib`) |
| LTO | Link-Time Optimization support |
| Dead code elimination | `-gc-sections` / `--dead-strip` |
| Symbol visibility | Hidden by default, explicit exports |
| Debug info | DWARF format support |

### 10.3 Build System Integration

The compiler must support:
- CMake toolchain files
- Custom sysroot specification
- Cross-compilation
- Multiple output formats (object, assembly, LLVM IR/BC)

---

## 11. Zanna OS Kernel and Userland Requirements

This section documents the specific compiler requirements for building the Zanna OS kernel and userland components. The kernel is a freestanding AArch64 hybrid kernel, and the userland includes a custom libc and C++ standard library implementation.

### 11.1 OS Component Standards

| Component | Language | Standard | Mode | Notes |
|-----------|----------|----------|------|-------|
| Kernel | C++ | **C++20** | Freestanding | No exceptions, no RTTI |
| Bootloader (vboot) | C | **C17** | Freestanding | UEFI environment |
| libc | C | **C17** | Freestanding | Custom POSIX-like implementation |
| Userspace Programs | C++ | **C++20** | Hosted (custom libc++) | No exceptions, no RTTI |
| Host Tools | C++ | **C++17** | Hosted | Uses system STL |

### 11.2 Target Architecture

| Property | Value |
|----------|-------|
| Architecture | AArch64 (ARM64) |
| CPU Model | Cortex-A72 |
| Endianness | Little-endian |
| Pointer Size | 64-bit |
| Alignment | Strict (`-mstrict-align`) |

### 11.3 Kernel C++ Language Features

The kernel uses modern C++ but in a constrained freestanding environment:

#### 11.3.1 Core C++ Features Used

| Feature | Status | Example Usage |
|---------|--------|---------------|
| `auto` type deduction | YES | Throughout kernel code |
| `decltype` | YES | `using size_t = decltype(sizeof(0));` |
| Lambda expressions | YES | Callbacks in GIC, scheduler |
| `constexpr` functions | YES | Compile-time constants (150+ uses) |
| Range-based for loops | NO | Not used in kernel |
| Structured bindings | NO | Not used in kernel |
| `if constexpr` | NO | Uses preprocessor `#if` instead |
| Concepts/requires | NO | Not used |

#### 11.3.2 Template Features Used

| Feature | Example | Files |
|---------|---------|-------|
| Class templates | `template<typename T> class Result` | `lib/result.hpp` |
| Function templates | `template<typename T> T* as()` | `kobj/object.hpp` |
| Full specialization | `template<> class Result<void, E>` | `lib/result.hpp` |
| Variadic templates | Parameter packs | `main.cpp` |
| Non-type parameters | `template<size_t N>` | Various |

#### 11.3.3 Class Features Used

| Feature | Usage |
|---------|-------|
| Virtual functions | Extensive (30+ files) - Object hierarchy |
| `override` keyword | All virtual overrides |
| `final` keyword | Terminal classes |
| `= default` | Destructors, constructors |
| `= delete` | Prevent copy/move (e.g., `Spinlock`) |
| In-class initializers | `bool is_ok_ = false;` |
| Explicit constructors | `explicit operator bool()` |
| Multiple inheritance | Limited use |

#### 11.3.4 Memory Management

| Feature | Implementation |
|---------|---------------|
| `operator new` | Custom in `mm/kheap.hpp` |
| `operator new[]` | Custom in `mm/kheap.hpp` |
| `operator delete` | Custom with `noexcept` |
| Sized delete | `operator delete(void*, size_t)` |
| Placement new | Not used |
| `UniquePtr<T>` | Custom implementation in `mm/kheap.hpp` |
| `make_unique<T>` | Custom helper function |

#### 11.3.5 C++ Attributes Used

| Attribute | Syntax | Usage |
|-----------|--------|-------|
| `[[nodiscard]]` | C++17 | `Result<T>` methods (5+ uses) |
| `[[maybe_unused]]` | C++17 | Padding fields, debug helpers |
| `[[noreturn]]` | C++11 | `exit()`, `enter_user_mode()`, `kernel_panic()` |

#### 11.3.6 Other C++ Features

| Feature | Status | Notes |
|---------|--------|-------|
| Scoped enums (`enum class`) | YES | Extensively used (20+ files) |
| Unions | YES | `Result<T>` value/error union |
| `noexcept` specifier | YES | On `operator delete` |
| `static_assert` | YES | Structure validation |
| `mutable` members | YES | Cache/synchronization fields |
| Operator overloading | YES | Rights bitwise operators |
| `extern "C"` | YES | `kernel_main`, ABI compatibility |
| Nested namespaces | YES | `mm::cow`, `virtio::input` |
| Cast operators | YES | `static_cast`, `reinterpret_cast` |
| `volatile` keyword | YES | Device registers, asm contexts |

### 11.4 libc C Language Features

#### 11.4.1 C11/C17 Features Used

| Feature | Example | Files |
|---------|---------|-------|
| Fixed-width integers | `int8_t` - `uint64_t` | All libc files |
| `inline` functions | `static inline int fold_case()` | `fnmatch.c`, `fenv.c` |
| `restrict` keyword | `const char *restrict nptr` | `inttypes.c` |
| `_Static_assert` | Via macro fallback | `assert.h` |
| Variadic macros | `#define LOG(...)` | `stdarg.h` |
| Designated initializers | `.field = value` | Struct initialization |
| Compound literals | `(struct type){values}` | Limited use |
| `//` comments | Throughout | All files |
| `long long` type | 64-bit integers | `inttypes.c` |

#### 11.4.2 Type System

| Category | Types Required |
|----------|----------------|
| Fixed-width | `int8_t`, `int16_t`, `int32_t`, `int64_t`, `uint*_t` variants |
| Size types | `size_t`, `ssize_t`, `ptrdiff_t`, `intptr_t`, `uintptr_t` |
| Max types | `intmax_t`, `uintmax_t` |
| Fast types | `int_least8_t`, `int_fast8_t`, etc. |

#### 11.4.3 Function Pointer Types

The libc extensively uses function pointers for callbacks:

```c
// qsort/bsearch comparators
int (*compar)(const void *, const void *)

// Signal handlers
void (*handler)(int)

// Thread entry points
void *(*start_routine)(void *)

// File tree walk callbacks
int (*fn)(const char *, const struct stat *, int)
```

### 11.5 Compiler Builtins Required

#### 11.5.1 Core Builtins

| Builtin | Purpose | Files Using |
|---------|---------|-------------|
| `__builtin_offsetof(type, member)` | Struct field offset | `drivers/virtio/blk.cpp` |
| `__builtin_unreachable()` | Unreachable code marker | `syscall.hpp` |
| `__builtin_va_list` | Variadic argument type | `stdarg.h` |
| `__builtin_va_start(ap, last)` | Start variadic processing | `stdio.c` |
| `__builtin_va_arg(ap, type)` | Get next argument | `stdio.c` |
| `__builtin_va_end(ap)` | End variadic processing | `stdio.c` |
| `__builtin_va_copy(dest, src)` | Copy va_list | `stdio.c` |

#### 11.5.2 Builtins NOT Used (but recommended)

The following builtins are NOT currently used but may be beneficial:

| Builtin | Purpose |
|---------|---------|
| `__builtin_clz(x)` | Count leading zeros |
| `__builtin_ctz(x)` | Count trailing zeros |
| `__builtin_popcount(x)` | Population count |
| `__builtin_bswap16/32/64(x)` | Byte swap |
| `__builtin_expect(expr, val)` | Branch prediction |
| `__builtin_prefetch(addr)` | Cache prefetch |

### 11.6 GCC/Clang Attributes Required

#### 11.6.1 Type/Struct Attributes

| Attribute | Syntax | Purpose | Files |
|-----------|--------|---------|-------|
| `packed` | `__attribute__((packed))` | Remove struct padding | 56 instances (network headers, VirtIO) |
| `aligned(N)` | `__attribute__((aligned(N)))` | Explicit alignment | 16 instances (N=4,8,4096) |

#### 11.6.2 Variable Attributes

| Attribute | Syntax | Purpose | Files |
|-----------|--------|---------|-------|
| `unused` | `__attribute__((unused))` | Suppress warnings | 4 instances |
| `aligned(N)` | `__attribute__((aligned(N)))` | Buffer alignment | Network packet buffers |

#### 11.6.3 Function Attributes

| Attribute | Syntax | Purpose | Files |
|-----------|--------|---------|-------|
| `noreturn` | `__attribute__((noreturn))` | No-return function | `longjmp`, exit functions |

#### 11.6.4 Combined Attributes

```c
// DMA access structure requires both
__attribute__((packed, aligned(8)))
```

### 11.8 Assembly File Requirements

The OS includes 7 assembly files (`.S`) requiring:

#### 11.8.1 Section Directives

| Directive | Purpose | Example |
|-----------|---------|---------|
| `.section .text.boot` | Boot code | `boot.S` |
| `.section .text` | Executable code | All `.S` files |
| `.section .bss` | Uninitialized data | Stack space |
| `.section .rodata` | Read-only data | Constants |

#### 11.8.2 Alignment Directives

| Directive | Bytes | Purpose |
|-----------|-------|---------|
| `.align 11` | 2048 | VBAR_EL1 vector table |
| `.align 7` | 128 | Vector entry spacing |
| `.align 16` | 16 | Stack alignment |
| `.align 4` | 4 | Instruction alignment |

#### 11.8.3 Symbol Directives

| Directive | Purpose |
|-----------|---------|
| `.global name` | Export symbol |
| `.type name, @function` | Mark as function |
| `.size name, . - name` | Record size |
| `.equ NAME, value` | Define constant |

#### 11.8.4 Key Assembly Constructs

**Exception Vector Table Layout (2048 bytes, 16 entries × 128 bytes):**
```text
VBAR_EL1 → 0x000: SP0 sync/IRQ/FIQ/SError (invalid)
           0x200: SPx sync/IRQ/FIQ/SError (kernel mode)
           0x400: EL0 sync/IRQ/FIQ/SError (user mode)
           0x600: AArch32 (unsupported)
```

**ExceptionFrame Structure (288 bytes):**
```text
[0-238]:   x0-x29 (30 registers × 8 bytes)
[240-248]: x30 (LR), SP
[256-264]: ELR_EL1, SPSR_EL1
[272-280]: ESR_EL1, FAR_EL1
```

**TaskContext Structure (104 bytes):**
```text
[0x00-0x50]: x19-x29 (callee-saved), x30 (LR)
[0x60]:      SP (stack pointer)
```

### 11.9 Freestanding Mode Configuration

#### 11.9.1 Kernel Compiler Flags

```bash
# Standard
-std=c++20

# Freestanding mode
-ffreestanding
-fno-exceptions
-fno-rtti
-fno-threadsafe-statics
-fno-use-cxa-atexit
-fno-stack-protector

# AArch64-specific
-mcpu=cortex-a72
-mstrict-align
-mgeneral-regs-only    # No FPU in kernel context

# Linking
-nostdlib
-static
```

#### 11.9.2 Userspace Compiler Flags

```bash
# Standard
-std=c++20

# Freestanding with custom libc
-ffreestanding
-fno-exceptions
-fno-rtti
-fno-threadsafe-statics
-fno-use-cxa-atexit
-fno-stack-protector
-fno-builtin

# AArch64-specific
-mcpu=cortex-a72
-nostdinc
-nostdlib

# Linking
-static
```

#### 11.9.3 Bootloader (UEFI) Flags

```bash
# Standard
-std=c17

# UEFI-specific
-ffreestanding
-fno-stack-protector
-fno-stack-check
-fshort-wchar          # 16-bit wchar_t for UEFI
-fPIC                  # Position-independent code

# Linking
-nostdlib
-Wl,--no-dynamic-linker
-Wl,-pie
```

### 11.10 Custom Runtime Requirements

#### 11.10.1 Kernel C Runtime (`crt.cpp`)

The kernel provides freestanding implementations:

| Function | Purpose |
|----------|---------|
| `memcpy()` | Memory copy |
| `memset()` | Memory fill |
| `memmove()` | Overlapping copy |

#### 11.10.2 Required ABI Functions

**C++ ABI Functions (Freestanding - No Exceptions):**

| Function | Purpose | Required |
|----------|---------|----------|
| `__cxa_pure_virtual()` | Called when pure virtual function invoked | Yes |
| `__cxa_deleted_virtual()` | Called when deleted virtual function invoked | Yes |
| `__cxa_guard_acquire()` | Thread-safe static initialization (acquire) | No* |
| `__cxa_guard_release()` | Thread-safe static initialization (release) | No* |
| `__cxa_guard_abort()` | Thread-safe static initialization (abort) | No* |
| `__cxa_atexit()` | Register destructor for static object | No* |

*Not required when compiled with `-fno-threadsafe-statics` and `-fno-use-cxa-atexit`.

**Exception Handling Functions (NOT required with `-fno-exceptions`):**

| Function | Purpose | Required |
|----------|---------|----------|
| `__cxa_allocate_exception()` | Allocate exception object | No |
| `__cxa_free_exception()` | Free exception object | No |
| `__cxa_throw()` | Throw exception | No |
| `__cxa_begin_catch()` | Begin catch block | No |
| `__cxa_end_catch()` | End catch block | No |
| `__cxa_rethrow()` | Rethrow current exception | No |
| `__cxa_get_exception_ptr()` | Get exception pointer | No |
| `_Unwind_*` | Stack unwinding functions | No |

**Memory Operators:**

| Function | Purpose |
|----------|---------|
| `operator new(size_t)` | Single object allocation |
| `operator new[](size_t)` | Array allocation |
| `operator delete(void*)` | Single object deallocation |
| `operator delete[](void*)` | Array deallocation |
| `operator delete(void*, size_t)` | Sized deallocation (C++14) |
| `operator delete[](void*, size_t)` | Sized array deallocation (C++14) |

#### 11.10.3 Userspace C++ Runtime (`new.cpp`)

```cpp
void* operator new(size_t size);
void* operator new[](size_t size);
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void* operator new(size_t, const std::nothrow_t&) noexcept;
void operator delete(void*, size_t) noexcept;  // Sized delete
```

### 11.11 Cross-Compilation Toolchain

#### 11.11.1 Required Tools

| Tool | Purpose |
|------|---------|
| `clang` / `clang++` | Primary compiler (preferred) |
| `aarch64-elf-gcc` / `aarch64-elf-g++` | Alternative compiler |
| `aarch64-elf-ld` | GNU linker (required for both) |
| `aarch64-elf-objcopy` | Object file manipulation |
| `aarch64-elf-ar` | Archive creation |
| `aarch64-elf-ranlib` | Archive indexing |

#### 11.11.2 Clang Target Triple

```text
--target=aarch64-none-elf
```

#### 11.11.3 Linker Scripts

| Script | Purpose |
|--------|---------|
| `kernel/kernel.ld` | Kernel memory layout |
| `user/user.ld` | Userspace program layout |
| `vboot/vboot.ld` | UEFI bootloader layout |

### 11.12 Custom C++ Standard Library

The userspace includes a complete custom C++ standard library at `user/libc/include/c++/` with 68 headers providing:

#### 11.12.1 Containers
- `vector`, `string`, `array`, `deque`, `list`, `forward_list`
- `map`, `set`, `unordered_map`, `unordered_set`
- `queue`, `stack`, `bitset`, `span`

#### 11.12.2 Utilities
- `memory`, `functional`, `utility`, `iterator`
- `tuple`, `pair`, `optional`, `variant`, `any`
- `initializer_list`

#### 11.12.3 Algorithms
- `algorithm`, `numeric`

#### 11.12.4 Type Support
- `type_traits`, `limits`, `concepts`

#### 11.12.5 Threading (stubs)
- `thread`, `mutex`, `atomic`, `condition_variable`

---

## Appendix A: Feature Test Matrix

| Feature | GCC | Clang | MSVC | Notes |
|---------|-----|-------|------|-------|
| C++17 | 7+ | 5+ | 19.14+ | Structured bindings, optional, variant |
| C11 | 4.6+ | 3.1+ | N/A | MSVC has limited C11 support |
| Atomics | 4.7+ | 3.1+ | 19.0+ | GCC-style builtins |
| Thread-local | 4.8+ | 3.3+ | 19.0+ | `thread_local` keyword |
| constexpr | 4.6+ | 3.1+ | 19.0+ | Full C++14 constexpr: GCC 5+, Clang 3.4+ |

---

## Appendix B: Minimum Compiler Versions

| Compiler | Minimum Version | Recommended |
|----------|-----------------|-------------|
| Apple Clang | 10.0 | 16+ |
| Clang | 7.0 | 16+ |
| GCC | 8.0 | 12+ |

---

## Appendix C: Test Program

The following program can be used to verify compiler compliance:

```cpp
// vcpp_test.cpp - Compiler compliance test
#include <optional>
#include <variant>
#include <string_view>
#include <vector>
#include <memory>
#include <cstdint>

// C++17 structured bindings
std::pair<int, int> get_pair() { return {1, 2}; }

// Templates with SFINAE
template<typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
T double_value(T x) { return x * 2; }

// Lambda with captures
auto make_adder(int x) {
    return [x](int y) { return x + y; };
}

// constexpr
constexpr int factorial(int n) {
    return n <= 1 ? 1 : n * factorial(n - 1);
}

// thread_local
thread_local int tls_value = 0;

// noexcept and nodiscard
[[nodiscard]] int get_value() noexcept { return 42; }

int main() {
    // Structured binding
    auto [a, b] = get_pair();

    // Optional
    std::optional<int> opt = 42;

    // Variant
    std::variant<int, double> var = 3.14;

    // String view
    std::string_view sv = "hello";

    // Smart pointers
    auto ptr = std::make_unique<int>(42);

    // constexpr
    static_assert(factorial(5) == 120);

    // Lambda
    auto add5 = make_adder(5);

    return add5(a) + *opt + static_cast<int>(std::get<double>(var));
}
```

Compile with:
```bash
clang++ -std=c++20 -Wall -Wextra vcpp_test.cpp -o vcpp_test
```

---

*Document Version: 2.0*
*Updated with Zanna OS Kernel and Userland Requirements*
*Generated for Zanna Project System Compiler Specification*
