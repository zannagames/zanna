//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file TestHarness.hpp
/// @brief Minimal, dependency-free unit testing framework for the Zanna project.
///
/// @details This header provides a lightweight test harness that enables writing
/// and running unit tests without external dependencies like Google Test or Catch2.
/// The framework is intentionally simple to facilitate rapid compilation and
/// minimize build complexity.
///
/// ## Architecture Overview
///
/// The test harness consists of three main components:
///
/// ### 1. Test Registry
///
/// Tests are registered at static initialization time using the `TEST()` macro.
/// Each test case is stored in a global registry (implemented as a Meyer's
/// singleton vector) containing the suite name, test name, and test function:
///
/// ```cpp
/// TEST(MySuite, MyTest) {
///     EXPECT_EQ(1 + 1, 2);
/// }
/// ```
///
/// ### 2. Assertion Macros
///
/// Two families of assertion macros are provided:
///
/// | Macro Family | Behavior on Failure |
/// |--------------|---------------------|
/// | `EXPECT_*`   | Reports failure but continues test execution |
/// | `ASSERT_*`   | Reports failure and immediately aborts the test |
///
/// Available assertions:
/// - `EXPECT_TRUE(expr)` / `ASSERT_TRUE(expr)` — Verifies expression is truthy
/// - `EXPECT_FALSE(expr)` / `ASSERT_FALSE(expr)` — Verifies expression is falsy
/// - `EXPECT_EQ(a, b)` / `ASSERT_EQ(a, b)` — Equality (prints both values on failure)
/// - `EXPECT_NE(a, b)` / `ASSERT_NE(a, b)` — Inequality (prints both values)
/// - `EXPECT_GT(a, b)` / `ASSERT_GT(a, b)` — Greater than
/// - `EXPECT_LT(a, b)` / `ASSERT_LT(a, b)` — Less than
/// - `EXPECT_GE(a, b)` / `ASSERT_GE(a, b)` — Greater or equal
/// - `EXPECT_LE(a, b)` / `ASSERT_LE(a, b)` — Less or equal
/// - `EXPECT_NEAR(a, b, eps)` — Float near-equality
/// - `EXPECT_CONTAINS(str, substr)` — String contains substring
/// - `EXPECT_THROWS(expr, ExcType)` — Verifies exception is thrown
/// - `EXPECT_NO_THROW(expr)` — Verifies no exception is thrown
///
/// ### 3. Exception-Based Control Flow
///
/// Assertion failures and test skips are communicated via exceptions:
/// - `TestFailure` - Thrown on assertion failure (fatal flag controls abort)
/// - `TestSkip` - Thrown when `ZANNA_TEST_SKIP()` is called
///
/// ## Usage Example
///
/// ```cpp
/// #include "tests/TestHarness.hpp"
///
/// TEST(MathSuite, Addition) {
///     int result = 2 + 2;
///     ASSERT_EQ(result, 4);
///     EXPECT_TRUE(result > 0);
/// }
///
/// TEST(MathSuite, Division) {
///     if (!hasFpuSupport())
///         ZANNA_TEST_SKIP("FPU not available");
///     EXPECT_EQ(10.0 / 2.0, 5.0);
/// }
///
/// int main(int argc, char** argv) {
///     zanna_test::init(&argc, argv);
///     return zanna_test::run_all_tests();
/// }
/// ```
///
/// ## Output Format
///
/// Test results are printed to stdout/stderr in a format similar to Google Test:
///
/// ```
/// [  PASSED  ] MathSuite.Addition
/// [ SKIPPED  ] MathSuite.Division: FPU not available
/// [  FAILED  ] StringSuite.Parse
/// 1 test(s) failed.
/// 1 test(s) skipped.
/// ```
///
/// ### 4. Test Fixtures (TEST_F)
///
/// Tests that need shared setup/teardown can use fixtures:
///
/// ```cpp
/// class MyFixture : public zanna_test::TestFixture {
/// protected:
///     void SetUp() override { /* runs before each test */ }
///     void TearDown() override { /* runs after each test */ }
/// };
/// TEST_F(MyFixture, MyTest) { ... }
/// ```
///
/// ### 5. Command-Line Options
///
/// - `--filter=PATTERN` — Run only tests matching glob pattern (e.g., `--filter=ZiaLexer.*`)
/// - `--xml=PATH` — Write JUnit XML results to file for CI integration
///
/// ## Design Decisions
///
/// - **No external dependencies**: Compiles with just the standard library
/// - **Header-only**: No separate compilation unit required
/// - **Exception-based**: Uses C++ exceptions for control flow (requires RTTI)
/// - **Static registration**: Tests auto-register before main() runs
/// - **Compatible API**: Macro names mirror Google Test for familiarity
/// - **Value-printing**: Comparison failures show actual operand values
///
/// @see zanna_test::run_all_tests() - Entry point for test execution
/// @see zanna_test::TestCase - Test case descriptor structure
///
//===----------------------------------------------------------------------===//

#pragma once

#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdlib>
#include <windows.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
#endif

namespace zanna_test {

/// @brief Exception thrown when a test assertion fails.
///
/// @details This exception is caught by the test runner to determine whether
/// to continue executing the current test (non-fatal) or abort immediately
/// (fatal). The `fatal` flag distinguishes EXPECT failures (continue) from
/// ASSERT failures (abort).
///
/// ## Example
///
/// ```cpp
/// // EXPECT_TRUE throws TestFailure with fatal=false
/// // ASSERT_TRUE throws TestFailure with fatal=true
/// ```
///
/// @invariant Once thrown, the test runner will record the failure.
struct TestFailure final : public std::exception {
    /// @brief Construct a test failure exception.
    /// @param isFatal If true, the test runner will stop executing the current test.
    explicit TestFailure(bool isFatal) : fatal(isFatal) {}

    /// @brief Whether this failure should abort the current test.
    bool fatal = false;

    /// @brief Return a human-readable description of the failure.
    /// @return "fatal test failure" if fatal, otherwise "test failure".
    const char *what() const noexcept override {
        return fatal ? "fatal test failure" : "test failure";
    }
};

/// @brief Exception thrown to skip a test with an explanatory message.
///
/// @details Tests can call `ZANNA_TEST_SKIP("reason")` to indicate they
/// cannot run in the current environment (e.g., missing hardware, unsupported
/// platform). The test runner catches this exception and marks the test as
/// skipped rather than failed.
///
/// ## Example
///
/// ```cpp
/// TEST(Hardware, GpuCompute) {
///     if (!hasGpu())
///         ZANNA_TEST_SKIP("No GPU available");
///     // ... GPU tests ...
/// }
/// ```
struct TestSkip final : public std::exception {
    /// @brief Construct a skip exception with an explanatory reason.
    /// @param skipReason Human-readable explanation for why the test was skipped.
    explicit TestSkip(std::string skipReason) : reason(std::move(skipReason)) {}

    /// @brief The reason this test was skipped.
    std::string reason;

    /// @brief Return the skip reason.
    /// @return Pointer to the reason string.
    const char *what() const noexcept override {
        return reason.c_str();
    }
};

/// @brief Descriptor for a single test case in the registry.
///
/// @details Each test registered via the `TEST()` macro creates a TestCase
/// containing the suite name (for grouping), test name (for identification),
/// and a callable that executes the test body.
struct TestCase {
    std::string suite;        ///< Name of the test suite (first TEST argument).
    std::string name;         ///< Name of the individual test (second TEST argument).
    std::function<void()> fn; ///< The test function body to execute.
};

/// @brief Access the global test registry.
///
/// @details Returns a reference to the singleton vector that stores all
/// registered test cases. Tests are added to this registry at static
/// initialization time before main() runs. The registry uses Meyer's
/// singleton pattern to ensure proper initialization order.
///
/// @return Reference to the global test case vector.
inline std::vector<TestCase> &registry() {
    static std::vector<TestCase> tests;
    return tests;
}

/// @brief Helper struct that registers a test case at static initialization.
///
/// @details The `TEST()` macro creates a static instance of this struct,
/// causing the test to be added to the global registry before main() runs.
/// This pattern enables automatic test discovery without explicit registration.
struct TestRegistrar {
    /// @brief Register a test case in the global registry.
    ///
    /// @param suiteName Name of the test suite for grouping.
    /// @param testName Name of the individual test.
    /// @param fn Callable that executes the test body.
    TestRegistrar(std::string suiteName, std::string testName, std::function<void()> fn) {
        registry().push_back({std::move(suiteName), std::move(testName), std::move(fn)});
    }
};

/// @brief Access the global test filter pattern (empty = run all).
inline std::string &filter_pattern() {
    static std::string pattern;
    return pattern;
}

/// @brief Access the global JUnit XML output path (empty = disabled).
inline std::string &xml_output_path() {
    static std::string path;
    return path;
}

/// @brief Check if a test name matches a glob pattern.
///
/// @details Supports '*' as wildcard matching zero or more characters.
/// Pattern is matched against "Suite.TestName".
inline bool glob_match(std::string_view pattern, std::string_view text) {
    size_t pi = 0, ti = 0;
    size_t starP = std::string_view::npos, starT = 0;
    while (ti < text.size()) {
        if (pi < pattern.size() && (pattern[pi] == text[ti] || pattern[pi] == '?')) {
            ++pi;
            ++ti;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            starP = pi++;
            starT = ti;
        } else if (starP != std::string_view::npos) {
            pi = starP + 1;
            ti = ++starT;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*')
        ++pi;
    return pi == pattern.size();
}

/// @brief Parse command-line arguments for --filter and --xml flags.
inline void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg.substr(0, 9) == "--filter=")
            filter_pattern() = std::string(arg.substr(9));
        else if (arg.substr(0, 6) == "--xml=")
            xml_output_path() = std::string(arg.substr(6));
    }
}

/// @brief Initialize the test framework and parse command-line arguments.
///
/// @details Parses --filter=PATTERN and --xml=PATH arguments.
/// On Windows, suppresses debug assertion dialogs.
///
/// @param argc Pointer to argument count.
/// @param argv Pointer to argument array.
inline void init(int *argc, char ***argv) {
    if (argc && argv && *argv)
        parse_args(*argc, *argv);
#ifdef _WIN32
    // Suppress all MSVC debug dialogs so tests run non-interactively.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
#endif
}

/// @brief Initialize the test framework (overload for direct argv).
inline void init(int *argc, char **argv) {
    if (argc && argv)
        parse_args(*argc, argv);
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
#endif
}

/// @brief Base class for test fixtures.
///
/// @details Subclass this and override SetUp()/TearDown() to create shared
/// test state. Use with the TEST_F macro.
struct TestFixture {
    virtual ~TestFixture() = default;

    virtual void SetUp() {}

    virtual void TearDown() {}
};

/// @brief Report an assertion failure and throw a TestFailure exception.
///
/// @details Called by EXPECT_* and ASSERT_* macros when an assertion fails.
/// Prints the failed expression, source location, and then throws an exception
/// to signal the failure to the test runner.
///
/// @param expr String representation of the failed expression (from macro).
/// @param file Source file where the failure occurred (__FILE__).
/// @param line Line number where the failure occurred (__LINE__).
/// @param fatal If true, throw a fatal failure that aborts the test.
inline void report_failure(std::string_view expr, const char *file, int line, bool fatal) {
    std::cerr << file << ":" << line << ": failure\n";
    std::cerr << "  expression: " << expr << "\n";
    throw TestFailure(fatal);
}

/// @brief Helper for SFINAE void_t (portable across all C++ standard library versions).
template <typename...> using zanna_void_t = void;

/// @brief SFINAE helper: detect whether T can be streamed to std::ostream.
template <typename T, typename = void> struct is_streamable : std::false_type {};

template <typename T>
struct is_streamable<
    T,
    zanna_void_t<decltype(std::declval<std::ostream &>() << std::declval<const T &>())>>
    : std::true_type {};

template <typename T, typename = void> struct decays_to_function_pointer : std::false_type {};

template <typename T>
struct decays_to_function_pointer<T, zanna_void_t<decltype(+std::declval<const T &>())>>
    : std::bool_constant<
          std::is_pointer_v<decltype(+std::declval<const T &>())> &&
          std::is_function_v<std::remove_pointer_t<decltype(+std::declval<const T &>())>>> {};

template <typename T>
struct is_function_like_value
    : std::bool_constant<(std::is_pointer_v<std::remove_cv_t<std::remove_reference_t<T>>> &&
                          std::is_function_v<std::remove_pointer_t<
                              std::remove_cv_t<std::remove_reference_t<T>>>>) ||
                         decays_to_function_pointer<T>::value> {};

template <typename T>
inline auto value_to_string(const T &)
    -> std::enable_if_t<is_function_like_value<T>::value, std::string> {
    return "<function>";
}

/// @brief Convert a value to string for failure messages (streamable types).
template <typename T>
inline auto value_to_string(const T &val)
    -> std::enable_if_t<is_streamable<T>::value && !is_function_like_value<T>::value, std::string> {
    std::ostringstream oss;
    oss << val;
    return oss.str();
}

/// @brief Fallback for non-streamable types — returns a placeholder.
template <typename T>
inline auto value_to_string(const T &) -> std::enable_if_t<!is_streamable<T>::value, std::string> {
    return "<non-printable>";
}

/// @brief Report a comparison failure with actual values printed.
///
/// @details Called by EXPECT_EQ, EXPECT_GT, etc. when a comparison fails.
/// Uses SFINAE to print actual values when operator<< is available,
/// and falls back to "<non-printable>" otherwise.
template <typename A, typename B>
inline void report_comparison_failure(const char *op,
                                      const A &lhs,
                                      const B &rhs,
                                      const char *lhsExpr,
                                      const char *rhsExpr,
                                      const char *file,
                                      int line,
                                      bool fatal) {
    std::cerr << file << ":" << line << ": failure\n";
    std::cerr << "  expression: " << lhsExpr << " " << op << " " << rhsExpr << "\n";
    std::cerr << "  left value:  " << value_to_string(lhs) << "\n";
    std::cerr << "  right value: " << value_to_string(rhs) << "\n";
    throw TestFailure(fatal);
}

/// @brief Report a string containment failure.
inline void report_contains_failure(const std::string &haystack,
                                    const std::string &needle,
                                    const char *haystackExpr,
                                    const char *needleExpr,
                                    const char *file,
                                    int line,
                                    bool fatal) {
    std::cerr << file << ":" << line << ": failure\n";
    std::cerr << "  expression: " << haystackExpr << " contains " << needleExpr << "\n";
    std::cerr << "  haystack: \"" << haystack << "\"\n";
    std::cerr << "  needle:   \"" << needle << "\"\n";
    throw TestFailure(fatal);
}

/// @brief Skip the current test with an explanatory reason.
///
/// @details Called by the ZANNA_TEST_SKIP() macro to indicate that a test
/// cannot run in the current environment. This function never returns;
/// it always throws a TestSkip exception.
///
/// @param reason Human-readable explanation for why the test is being skipped.
[[noreturn]] inline void skip(std::string reason) {
    throw TestSkip(std::move(reason));
}

/// @brief Execute all registered tests and return the failure count.
///
/// @details Iterates through every test case in the global registry, executing
/// each test function and catching any exceptions to determine the outcome:
///
/// | Exception Type     | Outcome         | Continues? |
/// |--------------------|-----------------|------------|
/// | None               | PASSED          | Yes        |
/// | TestSkip           | SKIPPED         | Yes        |
/// | TestFailure(false) | FAILED          | Yes        |
/// | TestFailure(true)  | FAILED (abort)  | No         |
/// | std::exception     | FAILED          | Yes        |
/// | Unknown exception  | FAILED          | Yes        |
///
/// ## Output Format
///
/// Results are printed in a format compatible with CI systems:
/// ```
/// [  PASSED  ] Suite.Test
/// [ SKIPPED  ] Suite.Test: reason
/// [  FAILED  ] Suite.Test
/// ```
///
/// ## Usage
///
/// ```cpp
/// int main(int argc, char** argv) {
///     zanna_test::init(&argc, argv);
///     return zanna_test::run_all_tests();
/// }
/// ```
///
/// @return Number of failed tests (0 indicates all tests passed or were skipped).
/// @brief Escape a string for XML attribute/text content.
inline std::string xml_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

/// @brief Result of a single test case execution (for JUnit XML reporting).
struct TestResult {
    std::string suite;
    std::string name;

    enum Status { Passed, Failed, Skipped } status = Passed;

    std::string message;
};

inline int run_all_tests() {
    int failures = 0;
    int skips = 0;
    int filtered = 0;
    std::vector<TestResult> results;
    const auto &pattern = filter_pattern();

    for (const auto &t : registry()) {
        // Apply filter if set.
        if (!pattern.empty()) {
            std::string fullName = t.suite + "." + t.name;
            if (!glob_match(pattern, fullName)) {
                ++filtered;
                continue;
            }
        }

        TestResult result;
        result.suite = t.suite;
        result.name = t.name;

        try {
            t.fn();
            result.status = TestResult::Passed;
            std::cout << "[  PASSED  ] " << t.suite << "." << t.name << "\n";
        } catch (const TestSkip &s) {
            ++skips;
            result.status = TestResult::Skipped;
            result.message = s.what();
            std::cout << "[ SKIPPED  ] " << t.suite << "." << t.name << ": " << s.what() << "\n";
        } catch (const TestFailure &f) {
            ++failures;
            result.status = TestResult::Failed;
            result.message = f.fatal ? "ASSERT failure" : "EXPECT failure";
            std::cerr << "[  FAILED  ] " << t.suite << "." << t.name;
            if (!f.fatal)
                std::cerr << " (non-fatal)";
            std::cerr << "\n";
            if (f.fatal) {
                std::cerr << "Stopping due to ASSERT failure.\n";
                results.push_back(result);
                break;
            }
        } catch (const std::exception &e) {
            ++failures;
            result.status = TestResult::Failed;
            result.message = std::string("unhandled exception: ") + e.what();
            std::cerr << "[  FAILED  ] " << t.suite << "." << t.name
                      << " (unhandled exception: " << e.what() << ")\n";
        } catch (...) {
            ++failures;
            result.status = TestResult::Failed;
            result.message = "unknown exception";
            std::cerr << "[  FAILED  ] " << t.suite << "." << t.name << " (unknown exception)\n";
        }
        results.push_back(result);
    }

    int total = static_cast<int>(results.size());
    if (failures != 0)
        std::cerr << failures << " test(s) failed.\n";
    if (skips != 0)
        std::cout << skips << " test(s) skipped.\n";
    if (filtered > 0)
        std::cout << filtered << " test(s) filtered out.\n";

    // Write JUnit XML if --xml was specified.
    const auto &xmlPath = xml_output_path();
    if (!xmlPath.empty()) {
        std::ofstream xml(xmlPath);
        if (xml.is_open()) {
            xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
            xml << "<testsuites tests=\"" << total << "\" failures=\"" << failures
                << "\" skipped=\"" << skips << "\">\n";
            xml << "  <testsuite name=\"zanna\" tests=\"" << total << "\" failures=\"" << failures
                << "\" skipped=\"" << skips << "\">\n";
            for (const auto &r : results) {
                xml << "    <testcase classname=\"" << xml_escape(r.suite) << "\" name=\""
                    << xml_escape(r.name) << "\"";
                if (r.status == TestResult::Passed) {
                    xml << " />\n";
                } else if (r.status == TestResult::Skipped) {
                    xml << ">\n      <skipped message=\"" << xml_escape(r.message)
                        << "\" />\n    </testcase>\n";
                } else {
                    xml << ">\n      <failure message=\"" << xml_escape(r.message)
                        << "\" />\n    </testcase>\n";
                }
            }
            xml << "  </testsuite>\n</testsuites>\n";
        }
    }

    return failures;
}

} // namespace zanna_test

//===----------------------------------------------------------------------===//
// Implementation Detail Macros
//===----------------------------------------------------------------------===//

/// @cond INTERNAL
/// Internal macro for token concatenation (indirection required for expansion).
#define ZANNA_TEST_DETAIL_CAT_INNER(a, b) a##b
/// Internal macro for token concatenation.
#define ZANNA_TEST_DETAIL_CAT(a, b) ZANNA_TEST_DETAIL_CAT_INNER(a, b)
/// @endcond

//===----------------------------------------------------------------------===//
// Test Definition Macro
//===----------------------------------------------------------------------===//

/// @def TEST(SuiteName, TestName)
/// @brief Define and register a test case.
///
/// @details Creates a function that will be called when `run_all_tests()` is
/// invoked. The test is automatically registered in the global registry at
/// static initialization time, before main() runs.
///
/// ## Example
///
/// ```cpp
/// TEST(MySuite, MyTest) {
///     int x = compute();
///     EXPECT_EQ(x, 42);
/// }
/// ```
///
/// @param SuiteName Identifier for the test suite (used for grouping).
/// @param TestName Identifier for this specific test.
#define TEST(SuiteName, TestName)                                                                  \
    static void ZANNA_TEST_DETAIL_CAT(SuiteName, TestName)();                                      \
    static const ::zanna_test::TestRegistrar ZANNA_TEST_DETAIL_CAT(SuiteName,                      \
                                                                   TestName##Registrar)(           \
        #SuiteName, #TestName, ZANNA_TEST_DETAIL_CAT(SuiteName, TestName));                        \
    static void ZANNA_TEST_DETAIL_CAT(SuiteName, TestName)()

//===----------------------------------------------------------------------===//
// Fixture Test Definition Macro
//===----------------------------------------------------------------------===//

/// @def TEST_F(FixtureClass, TestName)
/// @brief Define a test case that uses a fixture for shared setup/teardown.
///
/// @details Creates an instance of FixtureClass, calls SetUp(), runs the test
/// body, then calls TearDown(). The test body has access to all fixture members.
///
/// ## Example
///
/// ```cpp
/// class MyFixture : public zanna_test::TestFixture {
/// protected:
///     int value;
///     void SetUp() override { value = 42; }
///     void TearDown() override { /* cleanup */ }
/// };
///
/// TEST_F(MyFixture, CheckValue) {
///     EXPECT_EQ(value, 42);
/// }
/// ```
#define TEST_F(FixtureClass, TestName)                                                             \
    class ZANNA_TEST_DETAIL_CAT(FixtureClass, TestName) : public FixtureClass {                    \
      public:                                                                                      \
        void TestBody();                                                                           \
    };                                                                                             \
    static void ZANNA_TEST_DETAIL_CAT(FixtureClass, TestName##_Run)() {                            \
        ZANNA_TEST_DETAIL_CAT(FixtureClass, TestName) fixture;                                     \
        fixture.SetUp();                                                                           \
        try {                                                                                      \
            fixture.TestBody();                                                                    \
        } catch (...) {                                                                            \
            fixture.TearDown();                                                                    \
            throw;                                                                                 \
        }                                                                                          \
        fixture.TearDown();                                                                        \
    }                                                                                              \
    static const ::zanna_test::TestRegistrar ZANNA_TEST_DETAIL_CAT(FixtureClass,                   \
                                                                   TestName##Registrar)(           \
        #FixtureClass, #TestName, ZANNA_TEST_DETAIL_CAT(FixtureClass, TestName##_Run));            \
    void ZANNA_TEST_DETAIL_CAT(FixtureClass, TestName)::TestBody()

//===----------------------------------------------------------------------===//
// Expectation Macros (Non-Fatal Assertions)
//===----------------------------------------------------------------------===//

/// @def EXPECT_TRUE(expr)
/// @brief Assert that an expression evaluates to true (non-fatal).
///
/// @details If the expression is false, reports a failure but continues
/// executing the test. Use when you want to check multiple conditions
/// even if earlier ones fail.
///
/// @param expr Expression to evaluate.
#define EXPECT_TRUE(expr)                                                                          \
    do {                                                                                           \
        if (!(expr))                                                                               \
            ::zanna_test::report_failure(#expr, __FILE__, __LINE__, false);                        \
    } while (false)

/// @def EXPECT_FALSE(expr)
/// @brief Assert that an expression evaluates to false (non-fatal).
///
/// @param expr Expression to evaluate (should be false for the test to pass).
#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

// Suppress sign-compare warnings inside comparison macros. The old macros
// didn't capture values so the compiler only saw the == operator; the new
// value-printing macros bind to const auto& which can trigger -Wsign-compare
// when comparing signed and unsigned types (a very common pattern in tests).
#if defined(__clang__) || defined(__GNUC__)
#define ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wsign-compare\"")
#define ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END _Pragma("GCC diagnostic pop")
#elif defined(_MSC_VER)
#define ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
    __pragma(warning(push)) __pragma(warning(disable : 4018 4389))
#define ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END __pragma(warning(pop))
#else
#define ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN
#define ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END
#endif

/// @def EXPECT_EQ(val1, val2)
/// @brief Assert that two values are equal using operator== (non-fatal).
/// On failure, prints both actual values.
#define EXPECT_EQ(val1, val2)                                                                      \
    do {                                                                                           \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        if (!(zanna_test_lhs_ == zanna_test_rhs_))                                                 \
            ::zanna_test::report_comparison_failure(                                               \
                "==", zanna_test_lhs_, zanna_test_rhs_, #val1, #val2, __FILE__, __LINE__, false);  \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END                                                       \
    } while (false)

/// @def EXPECT_NE(val1, val2)
/// @brief Assert that two values are not equal using operator!= (non-fatal).
#define EXPECT_NE(val1, val2)                                                                      \
    do {                                                                                           \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        if (!(zanna_test_lhs_ != zanna_test_rhs_))                                                 \
            ::zanna_test::report_comparison_failure(                                               \
                "!=", zanna_test_lhs_, zanna_test_rhs_, #val1, #val2, __FILE__, __LINE__, false);  \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END                                                       \
    } while (false)

/// @def EXPECT_GT(val1, val2)
/// @brief Assert val1 > val2 (non-fatal). Prints both values on failure.
#define EXPECT_GT(val1, val2)                                                                      \
    do {                                                                                           \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        if (!(zanna_test_lhs_ > zanna_test_rhs_))                                                  \
            ::zanna_test::report_comparison_failure(                                               \
                ">", zanna_test_lhs_, zanna_test_rhs_, #val1, #val2, __FILE__, __LINE__, false);   \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END                                                       \
    } while (false)

/// @def EXPECT_LT(val1, val2)
/// @brief Assert val1 < val2 (non-fatal). Prints both values on failure.
#define EXPECT_LT(val1, val2)                                                                      \
    do {                                                                                           \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        if (!(zanna_test_lhs_ < zanna_test_rhs_))                                                  \
            ::zanna_test::report_comparison_failure(                                               \
                "<", zanna_test_lhs_, zanna_test_rhs_, #val1, #val2, __FILE__, __LINE__, false);   \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END                                                       \
    } while (false)

/// @def EXPECT_GE(val1, val2)
/// @brief Assert val1 >= val2 (non-fatal). Prints both values on failure.
#define EXPECT_GE(val1, val2)                                                                      \
    do {                                                                                           \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        if (!(zanna_test_lhs_ >= zanna_test_rhs_))                                                 \
            ::zanna_test::report_comparison_failure(                                               \
                ">=", zanna_test_lhs_, zanna_test_rhs_, #val1, #val2, __FILE__, __LINE__, false);  \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END                                                       \
    } while (false)

/// @def EXPECT_LE(val1, val2)
/// @brief Assert val1 <= val2 (non-fatal). Prints both values on failure.
#define EXPECT_LE(val1, val2)                                                                      \
    do {                                                                                           \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        if (!(zanna_test_lhs_ <= zanna_test_rhs_))                                                 \
            ::zanna_test::report_comparison_failure(                                               \
                "<=", zanna_test_lhs_, zanna_test_rhs_, #val1, #val2, __FILE__, __LINE__, false);  \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END                                                       \
    } while (false)

/// @def EXPECT_NEAR(val1, val2, epsilon)
/// @brief Assert |val1 - val2| <= epsilon (non-fatal). For floating-point comparisons.
#define EXPECT_NEAR(val1, val2, epsilon)                                                           \
    do {                                                                                           \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        const auto zanna_test_eps_ = (epsilon);                                                    \
        if (std::fabs(static_cast<double>(zanna_test_lhs_) -                                       \
                      static_cast<double>(zanna_test_rhs_)) >                                      \
            static_cast<double>(zanna_test_eps_)) {                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << ": failure\n";                             \
            std::cerr << "  expression: |" #val1 " - " #val2 "| <= " #epsilon "\n";                \
            std::cerr << "  left value:  " << zanna_test_lhs_ << "\n";                             \
            std::cerr << "  right value: " << zanna_test_rhs_ << "\n";                             \
            std::cerr << "  epsilon:     " << zanna_test_eps_ << "\n";                             \
            throw ::zanna_test::TestFailure(false);                                                \
        }                                                                                          \
    } while (false)

/// @def EXPECT_CONTAINS(haystack, needle)
/// @brief Assert that haystack string contains needle (non-fatal).
#define EXPECT_CONTAINS(haystack, needle)                                                          \
    do {                                                                                           \
        const std::string zanna_test_h_(haystack);                                                 \
        const std::string zanna_test_n_(needle);                                                   \
        if (zanna_test_h_.find(zanna_test_n_) == std::string::npos)                                \
            ::zanna_test::report_contains_failure(                                                 \
                zanna_test_h_, zanna_test_n_, #haystack, #needle, __FILE__, __LINE__, false);      \
    } while (false)

/// @def EXPECT_THROWS(expr, ExcType)
/// @brief Assert that expr throws an exception of type ExcType (non-fatal).
#define EXPECT_THROWS(expr, ExcType)                                                               \
    do {                                                                                           \
        bool zanna_test_caught_ = false;                                                           \
        try {                                                                                      \
            (void)(expr);                                                                          \
        } catch (const ExcType &) {                                                                \
            zanna_test_caught_ = true;                                                             \
        } catch (...) {                                                                            \
        }                                                                                          \
        if (!zanna_test_caught_)                                                                   \
            ::zanna_test::report_failure(#expr " throws " #ExcType, __FILE__, __LINE__, false);    \
    } while (false)

/// @def EXPECT_NO_THROW(expr)
/// @brief Assert that expr does not throw any exception (non-fatal).
#define EXPECT_NO_THROW(expr)                                                                      \
    do {                                                                                           \
        try {                                                                                      \
            (void)(expr);                                                                          \
        } catch (...) {                                                                            \
            ::zanna_test::report_failure(#expr " should not throw", __FILE__, __LINE__, false);    \
        }                                                                                          \
    } while (false)

//===----------------------------------------------------------------------===//
// Assertion Macros (Fatal Assertions)
//===----------------------------------------------------------------------===//

/// @def ASSERT_TRUE(expr)
/// @brief Assert that an expression evaluates to true (fatal).
#define ASSERT_TRUE(expr)                                                                          \
    do {                                                                                           \
        if (!(expr))                                                                               \
            ::zanna_test::report_failure(#expr, __FILE__, __LINE__, true);                         \
    } while (false)

/// @def ASSERT_FALSE(expr)
/// @brief Assert that an expression evaluates to false (fatal).
#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

/// @def ASSERT_EQ(val1, val2)
/// @brief Assert that two values are equal using operator== (fatal). Prints values on failure.
#define ASSERT_EQ(val1, val2)                                                                      \
    do {                                                                                           \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        if (!(zanna_test_lhs_ == zanna_test_rhs_))                                                 \
            ::zanna_test::report_comparison_failure(                                               \
                "==", zanna_test_lhs_, zanna_test_rhs_, #val1, #val2, __FILE__, __LINE__, true);   \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END                                                       \
    } while (false)

/// @def ASSERT_NE(val1, val2)
/// @brief Assert that two values are not equal using operator!= (fatal).
#define ASSERT_NE(val1, val2)                                                                      \
    do {                                                                                           \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        if (!(zanna_test_lhs_ != zanna_test_rhs_))                                                 \
            ::zanna_test::report_comparison_failure(                                               \
                "!=", zanna_test_lhs_, zanna_test_rhs_, #val1, #val2, __FILE__, __LINE__, true);   \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END                                                       \
    } while (false)

/// @def ASSERT_GT(val1, val2)
/// @brief Assert val1 > val2 (fatal).
#define ASSERT_GT(val1, val2)                                                                      \
    do {                                                                                           \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        if (!(zanna_test_lhs_ > zanna_test_rhs_))                                                  \
            ::zanna_test::report_comparison_failure(                                               \
                ">", zanna_test_lhs_, zanna_test_rhs_, #val1, #val2, __FILE__, __LINE__, true);    \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END                                                       \
    } while (false)

/// @def ASSERT_LT(val1, val2)
/// @brief Assert val1 < val2 (fatal).
#define ASSERT_LT(val1, val2)                                                                      \
    do {                                                                                           \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        if (!(zanna_test_lhs_ < zanna_test_rhs_))                                                  \
            ::zanna_test::report_comparison_failure(                                               \
                "<", zanna_test_lhs_, zanna_test_rhs_, #val1, #val2, __FILE__, __LINE__, true);    \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END                                                       \
    } while (false)

/// @def ASSERT_GE(val1, val2)
/// @brief Assert val1 >= val2 (fatal).
#define ASSERT_GE(val1, val2)                                                                      \
    do {                                                                                           \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        if (!(zanna_test_lhs_ >= zanna_test_rhs_))                                                 \
            ::zanna_test::report_comparison_failure(                                               \
                ">=", zanna_test_lhs_, zanna_test_rhs_, #val1, #val2, __FILE__, __LINE__, true);   \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END                                                       \
    } while (false)

/// @def ASSERT_LE(val1, val2)
/// @brief Assert val1 <= val2 (fatal).
#define ASSERT_LE(val1, val2)                                                                      \
    do {                                                                                           \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_BEGIN                                                     \
        const auto zanna_test_lhs_ = (val1);                                                       \
        const auto zanna_test_rhs_ = (val2);                                                       \
        if (!(zanna_test_lhs_ <= zanna_test_rhs_))                                                 \
            ::zanna_test::report_comparison_failure(                                               \
                "<=", zanna_test_lhs_, zanna_test_rhs_, #val1, #val2, __FILE__, __LINE__, true);   \
        ZANNA_TEST_SUPPRESS_SIGN_COMPARE_END                                                       \
    } while (false)

//===----------------------------------------------------------------------===//
// Test Skip Macro
//===----------------------------------------------------------------------===//

/// @def ZANNA_TEST_SKIP(reason)
/// @brief Skip the current test with an explanatory reason.
///
/// @details Use this macro when a test cannot run in the current environment
/// (e.g., missing hardware, unsupported platform, or unmet preconditions).
/// The test will be marked as skipped rather than failed.
///
/// ## Example
///
/// ```cpp
/// TEST(Network, WebSocket) {
///     if (!networkAvailable())
///         ZANNA_TEST_SKIP("No network connection");
///     // ... network tests ...
/// }
/// ```
///
/// @param reason Human-readable string explaining why the test is skipped.
#define ZANNA_TEST_SKIP(reason)                                                                    \
    do {                                                                                           \
        ::zanna_test::skip(reason);                                                                \
    } while (false)
