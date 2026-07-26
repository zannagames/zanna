//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file cmd_asset.hpp
/// @brief Declares offline 3D asset baking, validation, and usage entry points.
///
/// Entry points are stateless and receive their argument vector positioned at the asset
/// subcommand token.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdio>

/// @brief `zanna asset <bake|validate> ...`; returns the process exit code.
/// @param argc Number of arguments beginning with @c bake or @c validate.
/// @param argv Argument vector.
/// @return Zero on success, one on usage error, or two on asset-processing failure.
int cmdAsset(int argc, char **argv);

/// @brief Print `zanna asset` usage to @p out (used by `zanna help asset`).
/// @param out Destination C stream.
/// @return Zero after writing the usage text.
int cmdAssetHelp(std::FILE *out);
