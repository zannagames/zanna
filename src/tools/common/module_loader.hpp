//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/common/module_loader.hpp
// Purpose: Shared helpers for loading and verifying IL modules used by CLI tools.
// Key invariants: LoadResult accurately describes success or failure without mutating the output
// Ownership/Lifetime: Functions take Module by reference and populate it on success.
//                     Callers own the Module and must keep it alive while using returned results.
//                     LoadResult owns its diagnostic data; safe to copy/move.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares uniform IL module loading, verification, and reporting helpers.
/// @details Command-line tools use one structured result type to distinguish
///          file, parse, and verifier failures while optionally printing the
///          same retained diagnostic through the canonical support renderer.

#pragma once

#include "il/core/Module.hpp"
#include "support/diag_expected.hpp"

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace il::tools::common {

/// @brief Result classifications for attempting to load a module from disk.
/// @details Values distinguish the pipeline stage that stopped processing;
///          success means every stage explicitly requested by the caller passed.
enum class LoadStatus {
    Success,    ///< Module loaded successfully.
    FileError,  ///< Input file could not be opened.
    ParseError, ///< Parser reported diagnostics.
    VerifyError ///< Verifier reported diagnostics.
};

/// @brief Outcome produced by ::loadModuleFromFile describing the failure mode.
/// @details Failure results normally retain a structured diagnostic. The path is
///          populated by file-loading workflows but may be empty for standalone
///          verification through @ref verifyModuleResult.
struct LoadResult {
    LoadStatus status = LoadStatus::Success; ///< High-level status of the load.
    std::optional<il::support::Diag> diag{}; ///< Populated when parsing or verification fails.
    std::string path{};                      ///< Path that was loaded (useful for file errors).

    /// @brief Convenience for checking success.
    /// @return True only when @ref status is @ref LoadStatus::Success.
    [[nodiscard]] bool succeeded() const {
        return status == LoadStatus::Success;
    }

    /// @brief Check if the failure was due to file I/O.
    /// @return True only for @ref LoadStatus::FileError.
    [[nodiscard]] bool isFileError() const {
        return status == LoadStatus::FileError;
    }

    /// @brief Check if the failure was due to parsing.
    /// @return True only for @ref LoadStatus::ParseError.
    [[nodiscard]] bool isParseError() const {
        return status == LoadStatus::ParseError;
    }

    /// @brief Check if the failure was due to verification.
    /// @return True only for @ref LoadStatus::VerifyError.
    [[nodiscard]] bool isVerifyError() const {
        return status == LoadStatus::VerifyError;
    }

    /// @brief Human-readable description of the status category.
    /// @return Static lowercase phrase naming @ref status, or `"unknown"` for
    ///         an unrecognized underlying value.
    [[nodiscard]] const char *statusName() const {
        switch (status) {
            case LoadStatus::Success:
                return "success";
            case LoadStatus::FileError:
                return "file error";
            case LoadStatus::ParseError:
                return "parse error";
            case LoadStatus::VerifyError:
                return "verify error";
        }
        return "unknown";
    }
};

/// @brief Load an IL module from @p path, printing diagnostics to @p err.
///
/// @details On success the provided module is populated and the returned status
///          equals LoadStatus::Success. File opening, sizing, or seek failures
///          produce LoadStatus::FileError. Parser diagnostics are retained with
///          LoadStatus::ParseError. Output is written to @p err only when
///          @p printDiagnostics is true.
///
/// @param path Path to the IL text file to parse.
/// @param module Module receiving the parsed contents when successful.
/// @param err Stream receiving human-readable diagnostics.
/// @param ioErrorPrefix Prefix used when reporting file opening failures.
/// @param printDiagnostics True to print load/parse diagnostics during loading.
/// @return Structured result describing success or the failure category.
LoadResult loadModuleFromFile(const std::string &path,
                              il::core::Module &module,
                              std::ostream &err,
                              std::string_view ioErrorPrefix = "unable to open ",
                              bool printDiagnostics = true);

/// @brief Verify @p module and forward diagnostics to @p err when verification fails.
/// @param module Module to verify.
/// @param err Stream receiving diagnostics on error.
/// @param sm Optional source manager used to resolve diagnostic file paths.
/// @return True when verification succeeds; false otherwise.
/// @details Prints all verifier diagnostics, including warnings, and fails only
///          when at least one error-severity record is present.
bool verifyModule(const il::core::Module &module,
                  std::ostream &err,
                  const il::support::SourceManager *sm = nullptr);

/// @brief Verify @p module and return the result without printing.
/// @param module Module to verify.
/// @return LoadResult with VerifyError status on failure, Success otherwise.
/// @details Retains the first diagnostic supplied by the Expected verifier API.
LoadResult verifyModuleResult(const il::core::Module &module);

/// @brief Load and verify an IL module from @p path in one step.
///
/// @details Combines loadModuleFromFile and verifyModuleResult for tools that
///          want both parsing and verification with a single result type. The
///          first failed stage stops processing.
///
/// @param path Path to the IL text file to parse.
/// @param module Module receiving the parsed contents when successful.
/// @param sm Source manager used to resolve diagnostic file paths.
/// @param err Stream receiving human-readable diagnostics.
/// @param ioErrorPrefix Prefix used when reporting file opening failures.
/// @param printDiagnostics True to print load/verify diagnostics during loading.
/// @return Structured result describing success or the failure category.
LoadResult loadAndVerifyModule(const std::string &path,
                               il::core::Module &module,
                               const il::support::SourceManager *sm,
                               std::ostream &err,
                               std::string_view ioErrorPrefix = "unable to open ",
                               bool printDiagnostics = true);

/// @brief Print a LoadResult diagnostic to a stream.
/// @param result Result containing the diagnostic to print.
/// @param err Stream receiving the formatted diagnostic.
/// @param sm Optional source manager used to resolve diagnostic file paths.
/// @details Produces no output for success. Structured diagnostics use the
///          canonical printer; a diagnostic-less file error uses its stored path.
void printLoadResult(const LoadResult &result,
                     std::ostream &err,
                     const il::support::SourceManager *sm = nullptr);

} // namespace il::tools::common
