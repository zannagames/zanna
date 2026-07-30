//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_run_process_quotes.cpp
// Purpose: Verify shell-free subprocess argument, environment, output, and
//          working-directory behavior.
// Key invariants:
//   - Direct launch preserves argument bytes that native process APIs can
//     represent and rejects embedded NUL instead of truncating.
//   - Launch failure is distinct from a child that exits with a nonzero status.
// Ownership/Lifetime:
//   - RunResult owns captured output; each test removes any temporary
//     filesystem state it creates.
// Links: src/common/RunProcess.cpp, src/common/RunProcess.hpp,
//        docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#include "common/Filesystem.hpp"
#include "common/PlatformCapabilities.hpp"
#include "common/RunProcess.hpp"
#include "tests/TestHarness.hpp"
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
std::string trim_trailing_newlines(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}
} // namespace

namespace zanna::test_support {
bool windows_utf8_length_is_representable(std::size_t length) noexcept;
std::optional<std::size_t> windows_quoted_length(std::wstring_view argument) noexcept;
bool windows_command_line_argument_fits(std::size_t current,
                                        std::wstring_view argument,
                                        bool needs_separator) noexcept;
bool windows_utf8_argument_may_fit(std::size_t byte_length) noexcept;
void set_run_process_capture_failure_after_bytes(std::size_t limit) noexcept;

struct ScopedEnvironmentAssignmentMoveResult {
    bool value_visible_after_move_ctor;
    bool value_visible_after_move_assign;
    bool restored;
    std::optional<std::string> move_assigned_value;
};

ScopedEnvironmentAssignmentMoveResult scoped_environment_assignment_move_preserves(
    const std::string &name, const std::string &source_value, const std::string &receiver_value);
} // namespace zanna::test_support

TEST(RunProcess, ChecksWindowsUtf8ConversionLengthBeforeNarrowing) {
    const std::size_t maximum = static_cast<std::size_t>(INT_MAX);

    EXPECT_TRUE(zanna::test_support::windows_utf8_length_is_representable(maximum));
    EXPECT_FALSE(zanna::test_support::windows_utf8_length_is_representable(maximum + 1U));
}

TEST(RunProcess, EnforcesWindowsCommandLineLimitBeforeQuoting) {
    constexpr std::size_t maximumContent = 32766;
    constexpr std::size_t maximumUtf8BytesThatCouldFit = maximumContent * 3;

    const std::wstring unquotedAtLimit(maximumContent, L'a');
    const std::wstring unquotedOverLimit(maximumContent + 1U, L'a');
    EXPECT_TRUE(zanna::test_support::windows_command_line_argument_fits(0, unquotedAtLimit, false));
    EXPECT_FALSE(
        zanna::test_support::windows_command_line_argument_fits(0, unquotedOverLimit, false));

    std::wstring quotedAtLimit(maximumContent - 2U, L'a');
    quotedAtLimit.back() = L' ';
    std::wstring quotedOverLimit(maximumContent - 1U, L'a');
    quotedOverLimit.back() = L' ';
    EXPECT_TRUE(zanna::test_support::windows_command_line_argument_fits(0, quotedAtLimit, false));
    EXPECT_FALSE(
        zanna::test_support::windows_command_line_argument_fits(0, quotedOverLimit, false));

    const std::wstring escapedQuote = L"\\\"";
    const auto escapedLength = zanna::test_support::windows_quoted_length(escapedQuote);
    ASSERT_TRUE(escapedLength.has_value());
    EXPECT_EQ(static_cast<std::size_t>(6), *escapedLength);
    EXPECT_TRUE(zanna::test_support::windows_command_line_argument_fits(
        maximumContent - 7U, escapedQuote, true));
    EXPECT_FALSE(zanna::test_support::windows_command_line_argument_fits(
        maximumContent - 6U, escapedQuote, true));

    EXPECT_TRUE(zanna::test_support::windows_utf8_argument_may_fit(maximumUtf8BytesThatCouldFit));
    EXPECT_FALSE(
        zanna::test_support::windows_utf8_argument_may_fit(maximumUtf8BytesThatCouldFit + 1U));
}

TEST(RunProcess, PreservesQuotesAndBackslashes) {
    const std::string trickyArg = "value \"with quotes\" and backslash \\\\ tail";

    const RunResult result = run_process({"cmake", "-E", "echo", trickyArg});

    EXPECT_NE(-1, result.exit_code);
    EXPECT_EQ(trickyArg, trim_trailing_newlines(result.out));
}

#if !ZANNA_HOST_WINDOWS
TEST(RunProcess, EscapesPosixShellExpansions) {
    const std::string trickyArg = "literal $PATH and `uname` markers";

    const RunResult result = run_process({"cmake", "-E", "echo", trickyArg});

    EXPECT_NE(-1, result.exit_code);
    EXPECT_EQ(trickyArg, trim_trailing_newlines(result.out));
}
#endif

TEST(RunProcess, ForwardsEnvironmentVariables) {
    const std::string varName = "ZANNA_RUN_PROCESS_TEST_VAR";
    const std::string varValue = "zanna-test-value";
    const RunResult result =
        run_process({"cmake", "-E", "environment"}, std::nullopt, {{varName, varValue}});

    EXPECT_NE(-1, result.exit_code);
    const std::string expectedLine = varName + "=" + varValue;
    EXPECT_NE(std::string::npos, result.out.find(expectedLine));
}

TEST(RunProcess, ExplicitShellModeIsSeparate) {
    const RunResult result = run_shell_command("cmake -E echo zanna-shell-mode");

    EXPECT_NE(-1, result.exit_code);
    EXPECT_TRUE(result.launched);
    EXPECT_FALSE(result.launch_failed);
    EXPECT_EQ("zanna-shell-mode", trim_trailing_newlines(result.out));
}

TEST(RunProcess, MissingExecutableIsLaunchFailure) {
    const RunResult result = run_process({"zanna-definitely-missing-executable-for-run-process"});

    EXPECT_EQ(-1, result.exit_code);
    EXPECT_FALSE(result.launched);
    EXPECT_TRUE(result.launch_failed);
    EXPECT_FALSE(result.err.empty());
}

TEST(RunProcess, RejectsInvalidEnvironmentName) {
    const RunResult result =
        run_process({"cmake", "-E", "echo", "not-run"}, std::nullopt, {{"BAD=NAME", "value"}});

    EXPECT_EQ(-1, result.exit_code);
    EXPECT_FALSE(result.launched);
    EXPECT_TRUE(result.launch_failed);
    EXPECT_NE(std::string::npos, result.err.find("invalid environment variable name"));
}

TEST(RunProcess, RejectsEmbeddedNulArgument) {
    const std::string malformed{"visible\0hidden", 14};
    const RunResult result = run_process({"cmake", "-E", "echo", malformed});

    EXPECT_EQ(-1, result.exit_code);
    EXPECT_FALSE(result.launched);
    EXPECT_TRUE(result.launch_failed);
    EXPECT_NE(std::string::npos, result.err.find("embedded NUL"));
}

TEST(RunProcess, RejectsEmbeddedNulEnvironmentValue) {
    const std::string malformed{"before\0after", 12};
    const RunResult result =
        run_process({"cmake", "-E", "environment"}, std::nullopt, {{"ZANNA_NUL_TEST", malformed}});

    EXPECT_EQ(-1, result.exit_code);
    EXPECT_FALSE(result.launched);
    EXPECT_TRUE(result.launch_failed);
    EXPECT_NE(std::string::npos, result.err.find("embedded NUL"));
}

TEST(RunProcess, RejectsEmbeddedNulWorkingDirectory) {
    std::string malformed = zanna::filesystem::pathToUtf8(std::filesystem::temp_directory_path());
    malformed.append("\0ignored", 8);
    const RunResult result = run_process({"cmake", "-E", "echo", "not-run"}, malformed);

    EXPECT_EQ(-1, result.exit_code);
    EXPECT_FALSE(result.launched);
    EXPECT_TRUE(result.launch_failed);
    EXPECT_NE(std::string::npos, result.err.find("embedded NUL"));
}

TEST(RunProcess, ScopedEnvironmentAssignmentSurvivesMove) {
    const std::string varName = "ZANNA_SCOPED_ENV_MOVE_TEST";
    const std::string varValue = "scoped-env-move-value";

    const auto result = zanna::test_support::scoped_environment_assignment_move_preserves(
        varName, varValue, varValue);

    EXPECT_TRUE(result.value_visible_after_move_ctor);
    EXPECT_TRUE(result.value_visible_after_move_assign);
    EXPECT_TRUE(result.restored);
}

TEST(RunProcess, ScopedEnvironmentAssignmentMoveAssignmentPrefersSourceValue) {
    const std::string varName = "ZANNA_SCOPED_ENV_MOVE_ASSIGN_TEST";
    const std::string sourceValue = "scoped-env-source-value";
    const std::string receiverValue = "scoped-env-receiver-value";

    const auto result = zanna::test_support::scoped_environment_assignment_move_preserves(
        varName, sourceValue, receiverValue);

    EXPECT_TRUE(result.value_visible_after_move_ctor);
    ASSERT_TRUE(result.move_assigned_value.has_value());
    EXPECT_EQ(sourceValue, *result.move_assigned_value);
    EXPECT_TRUE(result.value_visible_after_move_assign);
    EXPECT_TRUE(result.restored);
}

TEST(RunProcess, AppliesWorkingDirectory) {
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path();
    const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
#if ZANNA_HOST_WINDOWS
    const std::filesystem::path tempDir =
        tempRoot /
        (std::wstring(L"zanna-run-process-\u6771\u4eac-") + std::to_wstring(uniqueSuffix));
#else
    const std::filesystem::path tempDir =
        tempRoot / std::filesystem::path("zanna-run-process-" + std::to_string(uniqueSuffix));
#endif

    std::filesystem::create_directories(tempDir);

    const RunResult result =
        run_process({"cmake", "-E", "touch", "marker.txt"}, zanna::filesystem::pathToUtf8(tempDir));

    EXPECT_NE(-1, result.exit_code);
    EXPECT_TRUE(std::filesystem::exists(tempDir / "marker.txt"));

    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
}

#if !ZANNA_HOST_WINDOWS
TEST(RunProcess, ReportsPosixExitStatus) {
    const RunResult result = run_process({"sh", "-c", "exit 42"});

    EXPECT_TRUE(result.launched);
    EXPECT_FALSE(result.launch_failed);
    EXPECT_EQ(42, result.exit_code);
}

TEST(RunProcess, DistinguishesPosixExit127FromLaunchFailure) {
    const RunResult result = run_process({"sh", "-c", "exit 127"});

    EXPECT_TRUE(result.launched);
    EXPECT_FALSE(result.launch_failed);
    EXPECT_EQ(127, result.exit_code);
}

TEST(RunProcess, SeparatesStdoutAndStderr) {
    const RunResult result =
        run_process({"sh", "-c", "printf stdout-only; printf stderr-only >&2"});

    EXPECT_TRUE(result.launched);
    EXPECT_FALSE(result.launch_failed);
    EXPECT_EQ("stdout-only", result.out);
    EXPECT_EQ("stderr-only", result.err);
}
#else
TEST(RunProcess, CapturesWindowsStderr) {
    const RunResult result = run_process({"cmd", "/C", "echo zanna-stderr-sample 1>&2"});

    EXPECT_NE(-1, result.exit_code);
    const std::string trimmed = trim_trailing_newlines(result.err);
    EXPECT_NE(std::string::npos, trimmed.find("zanna-stderr-sample"));
    EXPECT_EQ("", trim_trailing_newlines(result.out));
}
#endif

TEST(RunProcess, CaptureStorageFailureStillReapsChild) {
    zanna::test_support::set_run_process_capture_failure_after_bytes(0);
    const RunResult result = run_process({"cmake", "-E", "echo", "discarded-output"});
    zanna::test_support::set_run_process_capture_failure_after_bytes(
        std::numeric_limits<std::size_t>::max());

    EXPECT_TRUE(result.launched);
    EXPECT_FALSE(result.launch_failed);
    EXPECT_TRUE(result.capture_failed);
    EXPECT_EQ(0, result.exit_code);
    EXPECT_TRUE(result.out.empty());
    EXPECT_NE(std::string::npos, result.err.find("capture storage exhausted"));

    const RunResult followup = run_process({"cmake", "-E", "echo", "capture-restored"});
    EXPECT_TRUE(followup.launched);
    EXPECT_FALSE(followup.capture_failed);
    EXPECT_EQ("capture-restored", trim_trailing_newlines(followup.out));
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
