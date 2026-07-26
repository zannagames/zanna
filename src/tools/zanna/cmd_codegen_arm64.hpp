//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file cmd_codegen_arm64.hpp
/// @brief Declaration of the `ilc codegen arm64` subcommand entry point.
///
/// The subcommand borrows argv strings, never modifies its IL input, and drives reusable AArch64
/// lowering, assembly, linking, and optional native execution. Generated files are written only to
/// paths selected by command-line flags, and diagnostics are emitted to standard error.

#pragma once

namespace zanna::tools::ilc {

/// @brief Execute the `ilc codegen arm64` subcommand.
/// @details Parses the command-line arguments for the arm64 backend, then
///          lowers IL to AArch64 assembly and optionally assembles/links native
///          output depending on flags such as `-S`, `-o`, and `-run-native`.
///          Errors are reported to stderr and surfaced via a non-zero return
///          code to align with the rest of the ilc toolchain.
/// @param argc Number of command-line arguments in @p argv.
/// @param argv Argument vector where argv[0] is the subcommand name.
/// @return Zero on success; non-zero on argument parsing, IO, or codegen errors.
int cmd_codegen_arm64(int argc, char **argv);

} // namespace zanna::tools::ilc
