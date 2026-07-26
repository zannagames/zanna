//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/module_loader.cpp
// Purpose: Standardise how command-line tools load, verify, and report on IL modules.
// Key invariants: Load results precisely encode whether the failure came from
//                 file handling, parsing, or later verification. File, parse,
//                 and verifier failures may all retain structured diagnostics.
// Ownership/Lifetime: The helpers operate on caller-owned modules, streams, and
//                     diagnostic sinks.  Temporary diagnostics are copied into
//                     @ref LoadResult so the caller can persist them beyond the
//                     function call.
// Links: src/tools/common/module_loader.hpp, docs/internals/codemap.md#tools
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Provides IL module loading and verification helpers for CLI tools.
/// @details Command-line utilities link this implementation to standardise how
///          modules are parsed, validated, and reported to users.

#include "tools/common/module_loader.hpp"

#include "common/Filesystem.hpp"
#include "il/api/expected_api.hpp"
#include "il/verify/Verifier.hpp"

#include <fstream>
#include <limits>

namespace il::tools::common {
namespace {
constexpr std::streamoff kMaxIlModuleSize = static_cast<std::streamoff>(256ULL * 1024 * 1024);

/// @brief Build a successful load result with no diagnostics attached.
///
/// @details Initialises the @ref LoadResult structure with
///          @ref LoadStatus::Success and leaves the diagnostic slot empty.  The
///          helper keeps the success path uniform so callers can rely on
///          @ref LoadResult::succeeded without worrying about stale diagnostic
///          payloads lingering from earlier attempts.
///
/// @param path Path that was successfully loaded.
/// @return A result with @ref LoadStatus::Success and an empty diagnostic.
LoadResult makeSuccess(const std::string &path) {
    return {LoadStatus::Success, std::nullopt, path};
}

/// @brief Create a load result describing an I/O failure.
///
/// @details File handling failures (missing input, permissions, oversized data,
///          or failed seeking) are recorded with the affected path. The
///          diagnostic slot is populated with a basic error message so callers
///          can use printLoadResult uniformly.
///
/// @param path Path that could not be opened.
/// @param message Human-readable description of the I/O failure.
/// @return Result tagged with @ref LoadStatus::FileError.
LoadResult makeFileError(const std::string &path, std::string message) {
    il::support::Diag diag{il::support::Severity::Error, std::move(message), {}, "V-IL-IO"};
    return {LoadStatus::FileError, diag, path};
}

/// @brief Create a load result populated with a parser diagnostic.
///
/// @details Copies @p diag into the result so the caller retains access to the
///          structured error even after the temporary parser object has been
///          destroyed. The resulting strings, notes, and fix-its are independently
///          owned by the returned result.
///
/// @param path Path that was being parsed.
/// @param diag Diagnostic emitted by the parser that explains the failure.
/// @return A result tagged with @ref LoadStatus::ParseError holding @p diag.
LoadResult makeParseError(const std::string &path, const il::support::Diag &diag) {
    return {LoadStatus::ParseError, diag, path};
}

/// @brief Create a load result populated with a verifier diagnostic.
///
/// @details Similar to makeParseError but tagged for verification failures.
///
/// @param diag Diagnostic emitted by the verifier that explains the failure.
/// @return A result tagged with @ref LoadStatus::VerifyError holding @p diag.
LoadResult makeVerifyError(const il::support::Diag &diag) {
    return {LoadStatus::VerifyError, diag, {}};
}
} // namespace

/// @brief Load a textual IL module with optional immediate diagnostic printing.
///
/// @details The loader performs a full round-trip from disk to in-memory
///          module:
///          1. Open @p path as an input stream and emit @p ioErrorPrefix
///             followed by the failing path when the open call fails. The file is
///             also rejected (as a file error) when it exceeds the 256 MB limit.
///          2. Invoke @ref il::api::v2::parse_text_expected to populate
///             @p module, preserving any diagnostics emitted by the parser via
///             the Expected interface.
///          3. On parse failure, forward the diagnostic to @p err through
///             @ref il::support::printDiag and return a parse-error
///             @ref LoadResult constructed by @ref makeParseError.
///          4. On success, return @ref makeSuccess so callers can optionally run
///             verification as a separate step.
///          The helper intentionally leaves verification to the caller because
///          some tools wish to parse without enforcing full structural
///          correctness.
///
/// @param path Filesystem path to the IL file.
/// @param module Module instance receiving parsed content on success.
/// @param err Stream for human-readable diagnostics.
/// @param ioErrorPrefix Prefix emitted before file-system error messages.
/// @param printDiagnostics True to print diagnostics immediately.
/// @return Load status describing success, file errors, or parse failures.
LoadResult loadModuleFromFile(const std::string &path,
                              il::core::Module &module,
                              std::ostream &err,
                              std::string_view ioErrorPrefix,
                              bool printDiagnostics) {
    std::ifstream input(zanna::filesystem::pathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!input) {
        std::string message = std::string(ioErrorPrefix) + path;
        if (printDiagnostics)
            err << message << '\n';
        return makeFileError(path, std::move(message));
    }
    const std::streamoff size = input.tellg();
    if (size < 0 || size > kMaxIlModuleSize) {
        std::string message = "IL module too large: " + path + " (limit: 256 MB)";
        if (printDiagnostics)
            err << message << '\n';
        return makeFileError(path, std::move(message));
    }
    input.seekg(0, std::ios::beg);
    if (!input) {
        std::string message = "cannot seek IL module: " + path;
        if (printDiagnostics)
            err << message << '\n';
        return makeFileError(path, std::move(message));
    }

    auto parsed = il::api::v2::parse_text_expected(input, module);
    if (!parsed) {
        auto diag = parsed.error();
        if (diag.loc.file_id == 0) {
            diag.notes.push_back(
                il::support::DiagnosticNote{{}, "while parsing IL module: " + path});
        }
        if (printDiagnostics)
            il::support::printDiag(diag, err);
        return makeParseError(path, diag);
    }

    return makeSuccess(path);
}

/// @brief Run the IL verifier and forward diagnostics to the caller.
///
/// @details Collects diagnostics with @ref il::verify::Verifier::verifyAll
///          (capped at 50 entries) and prints every one through
///          @ref il::support::printDiag, including warnings.  Only
///          error-severity diagnostics flip the return value to @c false, so a
///          module that emits warnings but no errors still verifies as
///          successful.  A @ref il::support::SourceManager pointer is accepted so
///          tools can render file identifiers as paths when available, closely
///          matching the output produced by the main compiler driver.  This
///          differs from @ref verifyModuleResult, which reports only the first
///          error via the Expected pipeline without printing.
///
/// @param module Module that should satisfy verifier invariants.
/// @param err Output stream receiving verifier diagnostics.
/// @param sm Optional source manager providing path lookups for diagnostics.
/// @return @c true when no error-severity diagnostics are produced, otherwise
///         @c false after printing diagnostics to @p err.
bool verifyModule(const il::core::Module &module,
                  std::ostream &err,
                  const il::support::SourceManager *sm) {
    bool hasError = false;
    for (const auto &diag : il::verify::Verifier::verifyAll(module, 50)) {
        if (diag.severity == il::support::Severity::Error)
            hasError = true;
        il::support::printDiag(diag, err, sm);
    }
    return !hasError;
}

/// @brief Verify a module and return the result without printing.
///
/// @details Wraps the verifier's Expected result into a LoadResult so callers
///          can handle verification failures using the same pattern as parse
///          and file errors.  The diagnostic is copied into the result for
///          later inspection or printing.
///
/// @param module Module that should satisfy verifier invariants.
/// @return LoadResult with Success or VerifyError status.
LoadResult verifyModuleResult(const il::core::Module &module) {
    auto verified = il::api::v2::verify_module_expected(module);
    if (!verified) {
        return makeVerifyError(verified.error());
    }
    return {LoadStatus::Success, std::nullopt, {}};
}

/// @brief Load and verify an IL module in one step.
///
/// @details Combines loadModuleFromFile and verifyModuleResult into a single
///          operation for tools that want comprehensive loading with uniform
///          error handling.  File I/O and parse errors are returned immediately;
///          verification errors are returned after successful parsing.
///
/// @param path Filesystem path to the IL file.
/// @param module Module instance receiving parsed content on success.
/// @param sm Source manager providing path lookups for diagnostics.
/// @param err Output stream receiving human-readable diagnostics.
/// @param ioErrorPrefix Prefix emitted before file-system error messages.
/// @param printDiagnostics True to print diagnostics immediately.
/// @return Load status describing success or the first failure encountered.
LoadResult loadAndVerifyModule(const std::string &path,
                               il::core::Module &module,
                               const il::support::SourceManager *sm,
                               std::ostream &err,
                               std::string_view ioErrorPrefix,
                               bool printDiagnostics) {
    auto loadResult = loadModuleFromFile(path, module, err, ioErrorPrefix, printDiagnostics);
    if (!loadResult.succeeded()) {
        return loadResult;
    }

    auto verifyResult = verifyModuleResult(module);
    if (!verifyResult.succeeded()) {
        verifyResult.path = path;
        if (printDiagnostics && verifyResult.diag) {
            il::support::printDiag(*verifyResult.diag, err, sm);
        }
        return verifyResult;
    }

    return loadResult;
}

/// @brief Print a LoadResult diagnostic to a stream.
///
/// @details Formats the diagnostic stored in @p result using the standard
///          printDiag format.  For file errors that lack a structured
///          diagnostic, emits a simple error message with the path.
///
/// @param result Result containing the diagnostic to print.
/// @param err Stream receiving the formatted diagnostic.
/// @param sm Optional source manager used to resolve diagnostic file paths.
void printLoadResult(const LoadResult &result,
                     std::ostream &err,
                     const il::support::SourceManager *sm) {
    if (result.succeeded()) {
        return;
    }

    if (result.diag) {
        il::support::printDiag(*result.diag, err, sm);
    } else if (result.isFileError()) {
        err << "error: unable to open " << result.path << '\n';
    }
}

} // namespace il::tools::common
