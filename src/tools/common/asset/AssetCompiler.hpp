//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/asset/AssetCompiler.hpp
// Purpose: Build-time asset compilation. Reads embed/pack directives from
//          ProjectConfig, resolves file paths, builds ZPAK blobs for .rodata
//          embedding, and writes standalone .zpak pack files.
//
// Key invariants:
//   - All source paths are resolved relative to ProjectConfig::rootDir.
//   - Directory entries are recursively enumerated.
//   - The embedded blob is a complete ZPAK archive (same format as .zpak files).
//   - Pack files are named <projectName>-<packName>.zpak.
//
// Ownership/Lifetime:
//   - AssetBundle owns its blob vector and pack file path list.
//   - Caller must keep ProjectConfig alive during compileAssets().
//
// Links: ZpakWriter.hpp (serialization), project_loader.hpp (input config)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares project asset archive compilation and object-file embedding.
/// @details Asset directives are resolved beneath the project root and serialized
///          in ZPAK format either as an in-memory blob or standalone pack files.
///          The API owns returned bytes and paths while borrowing configuration
///          and error-output references for each call.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace il::tools::common {
struct ProjectConfig;
}

namespace zanna::asset {

/// @brief Result of asset compilation.
/// @details The three pack metadata vectors are positionally aligned: each path
///          has the expected size and SHA-256 hash at the same index.
struct AssetBundle {
    /// @brief ZPAK-format blob to embed in `.rodata`, or empty when none is produced.
    std::vector<uint8_t> embeddedBlob;

    /// @brief Paths to generated `.zpak` pack files on disk.
    std::vector<std::string> packFilePaths;

    /// @brief Expected byte sizes for @ref packFilePaths entries.
    /// @details Cache validation uses these sizes with @ref packFileHashes to
    ///          detect truncated or externally modified pack files before
    ///          reusing a cached bundle.
    std::vector<std::uintmax_t> packFileSizes;

    /// @brief Expected SHA-256 hashes for @ref packFilePaths entries.
    std::vector<std::string> packFileHashes;
};

/// @brief Compile assets declared in a project configuration.
///
/// @details Resolves every `embed` entry into one in-memory ZPAK blob and writes
///          each nonempty `pack` group as a standalone archive. Source paths
///          must remain within the project root; a bounded process cache may
///          reuse unchanged inputs and validated output packs.
/// @param config Project configuration containing embed and pack directives.
/// @param outputDir Directory for generated `.zpak` files.
/// @param err Receives the failure reason; otherwise left unchanged.
/// @return AssetBundle on success, nullopt on failure.
std::optional<AssetBundle> compileAssets(const il::tools::common::ProjectConfig &config,
                                         const std::string &outputDir,
                                         std::string &err);

/// @brief Write a .o file containing the embedded blob as rodata symbols.
///
/// @details Uses Zanna's own ObjectFileWriter to produce a native object with:
///   zanna_asset_blob      → the ZPAK data bytes (.rodata)
///   zanna_asset_blob_size → uint64 size value (.rodata)
///
/// No external compiler or assembler is invoked.
/// @param blob ZPAK-format blob data.
/// @param outPath Path for the output native object file.
/// @param err Receives the failure reason; otherwise left unchanged.
/// @return True on success; false when no host writer exists or writing fails.
bool writeAssetBlobObject(const std::vector<uint8_t> &blob,
                          const std::string &outPath,
                          std::string &err);

} // namespace zanna::asset
