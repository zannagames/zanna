//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/io/rt_ide_primitives.h
// Purpose: Workspace, asset, manifest, and transactional edit helpers used by
//          Zanna Studio and editor-style tooling.
// Key invariants:
//   - Workspace edit targets are validated before any disk mutation is attempted.
//   - Rooted edits canonicalize every root and target and reject paths outside the declared
//     workspace boundary.
//   - Edit application stages complete same-directory replacements, rechecks live contents before
//     commit, and retains backups for best-effort batch rollback.
//   - File indexing applies built-in, caller, and nested .gitignore exclusions and caps complete
//     traversal at 100,000 accepted entries.
//   - Workspace/file-index helpers never depend on compiler-layer services.
// Ownership/Lifetime:
//   - Runtime strings and opaque collection/watcher arguments are borrowed for each call.
//   - Every returned map or sequence is a fresh runtime-managed object whose reference transfers
//     to the caller; returned owning sequences retain their element objects.
// Links: src/runtime/io/rt_ide_primitives.cpp (implementation),
//        src/runtime/io/rt_watcher.h (watcher events),
//        src/runtime/io/rt_asset.h (mounted asset lookup)
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares editor-facing workspace, asset, manifest, and edit primitives.
 * @details Exposes filtered file indexing and paging, ignore evaluation,
 * watcher batching, asset resolution, manifest parsing, and transactional
 * multi-file edit validation/application constrained to zero, one, or
 * multiple workspace roots.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Enumerate a filtered workspace tree into entry maps.
/// @details Recursively applies built-in exclusions, caller patterns, nested `.gitignore` files,
///          and an optional case-normalized extension allow-list. Permission-denied entries are
///          skipped, directory maps are optional, and output stops at 100,000 accepted entries.
/// @param root Borrowed runtime path of the workspace root.
/// @param extensions_csv Delimited extension allow-list; empty includes all file extensions.
/// @param excludes_csv Delimited additional gitignore-style patterns.
/// @param include_dirs Nonzero to include directory maps.
/// @return Fresh owning Seq of entry maps; empty when the root is invalid or traversal fails.
void *rt_workspace_file_index_enumerate(rt_string root,
                                        rt_string extensions_csv,
                                        rt_string excludes_csv,
                                        int8_t include_dirs);

/// @brief Create an explicitly owned recursive workspace traversal.
/// @param root Borrowed workspace root path.
/// @param extensions_csv Delimited extension allow-list.
/// @param excludes_csv Delimited additional ignore patterns.
/// @param include_dirs Non-zero to emit matching directories.
/// @return Opaque cursor handle, or NULL when the root/traversal is invalid.
void *rt_workspace_file_index_cursor_new(rt_string root,
                                         rt_string extensions_csv,
                                         rt_string excludes_csv,
                                         int8_t include_dirs);

/// @brief Test whether an explicit workspace traversal handle is valid.
/// @param handle Borrowed cursor handle.
/// @return Non-zero for a non-NULL cursor.
int8_t rt_workspace_file_index_cursor_is_valid(void *handle);

/// @brief Return the immutable generation assigned to a traversal handle.
/// @param handle Borrowed cursor handle.
/// @return Positive generation, or zero for NULL.
int64_t rt_workspace_file_index_cursor_generation(void *handle);

/// @brief Advance an explicit traversal by one bounded page.
/// @param handle Borrowed cursor handle.
/// @param limit Maximum entries to emit, clamped to 1..4096.
/// @return Fresh result map tagged with the cursor generation.
void *rt_workspace_file_index_cursor_next(void *handle, int64_t limit);

/// @brief Destroy an explicit traversal handle.
/// @param handle Owned cursor handle; NULL is accepted.
void rt_workspace_file_index_cursor_destroy(void *handle);

/// @brief Return one bounded page of workspace file-index entries.
/// @details Applies the same validation, extension filtering, ignore rules, and
///          entry cap as @ref rt_workspace_file_index_enumerate, but emits at
///          most @p limit entries starting at logical match offset @p offset.
///          The result map contains validity/root fields, `entries`, `offset`, `limit`, `emitted`,
///          `nextOffset`, `scanned`, `maxEntries`, `done`, `truncated`, and `diagnostics`.
/// @param root Borrowed runtime path of the directory to enumerate.
/// @param extensions_csv Delimited extension filter, such as ".zia,.png".
/// @param excludes_csv Delimited additional ignore patterns.
/// @param include_dirs Non-zero to count matching directories as entries.
/// @param offset Zero-based logical match offset to start returning.
/// @param limit Maximum entries to emit; values outside 1..4096 are clamped.
/// @return Fresh page result map owned by the caller.
void *rt_workspace_file_index_page(rt_string root,
                                   rt_string extensions_csv,
                                   rt_string excludes_csv,
                                   int8_t include_dirs,
                                   int64_t offset,
                                   int64_t limit);

/// @brief Return status metadata for a workspace file-index traversal.
/// @details Applies the same root validation, extension filtering, ignore rules,
///          and entry cap as @ref rt_workspace_file_index_enumerate without
///          allocating one map per file. The result map contains `valid`, `root`,
///          `entryCount`, `maxEntries`, `truncated`, `fingerprint`, and `diagnostics`.
/// @param root Borrowed runtime path of the directory to enumerate.
/// @param extensions_csv Delimited extension filter, such as ".zia,.png".
/// @param excludes_csv Delimited additional ignore patterns.
/// @param include_dirs Non-zero to count matching directories as entries.
/// @return Fresh status map owned by the caller.
void *rt_workspace_file_index_status(rt_string root,
                                     rt_string extensions_csv,
                                     rt_string excludes_csv,
                                     int8_t include_dirs);

/// @brief Evaluate workspace ignore rules for one relative path.
/// @details Applies hard exclusions, caller patterns, and nested `.gitignore` files. A trailing
///          slash marks the candidate as a directory; an empty root uses the current directory.
/// @param root Borrowed runtime path of the workspace root.
/// @param relative_path Borrowed candidate path relative to @p root.
/// @param patterns Delimited caller ignore/negation patterns.
/// @return 1 when the path should be omitted; otherwise 0.
int8_t rt_workspace_file_index_should_ignore(rt_string root,
                                             rt_string relative_path,
                                             rt_string patterns);
/// @brief Poll a bounded batch of normalized workspace watcher events.
/// @details Each result map contains path, numeric and textual type, overflow count, and a
///          `requiresRescan` flag. Nonpositive limits default to 64.
/// @param watcher Borrowed opaque watcher handle; may be NULL.
/// @param max_events Maximum events to return, or a nonpositive value for the default.
/// @return Fresh owning Seq of event maps.
void *rt_workspace_watcher_poll_batch(void *watcher, int64_t max_events);

/// @brief Resolve an asset reference through filesystem and mounted-asset locations.
/// @details Searches absolute, scene-relative, project-relative, configured asset-root, and
///          mounted registry candidates in order. Relative scene and asset-root paths are anchored
///          to the project root.
/// @param scene_path Borrowed path of the referencing scene; may be empty.
/// @param project_root Borrowed project-root path; empty uses the current directory.
/// @param asset_roots_csv Delimited configured asset-root paths.
/// @param asset_path Borrowed asset reference to resolve.
/// @return Fresh result map with path/display/source, exists/found flags, and diagnostic text.
void *rt_asset_resolver_resolve(rt_string scene_path,
                                rt_string project_root,
                                rt_string asset_roots_csv,
                                rt_string asset_path);

/// @brief Parse project-manifest directive text.
/// @details Applies defaults, top-level directives, and run/build sections while collecting
///          unknown or malformed constructs into an owning diagnostics sequence.
/// @param text Borrowed runtime string containing the complete manifest.
/// @return Fresh manifest map with parsed fields and a `valid` flag.
void *rt_project_manifest_parse_text(rt_string text);

/// @brief Read and parse a project manifest file.
/// @param path Borrowed runtime path of the manifest.
/// @return Fresh parsed manifest map, or an invalid default manifest with diagnostics on failure.
void *rt_project_manifest_parse_file(rt_string path);

/// @brief Prepare an unrooted workspace edit batch without changing files.
/// @details Reads each target once and retains immutable original bytes, stable
///          file identities, canonical ranges, and diagnostics for one later
///          apply. The explicit handle must be destroyed by the caller.
/// @param edits Borrowed Seq of edit maps. `wholeFile=true` replaces a complete
///              target and may carry expectedMtime/expectedSize/expectedHash/maxBytes.
/// @return Owned prepared handle, including for failed validation.
void *rt_workspace_edit_prepare(void *edits);

/// @brief Prepare an edit batch constrained to one workspace root.
/// @param edits Borrowed Seq of edit maps.
/// @param root Existing directory that bounds all canonical targets.
/// @return Owned prepared handle, including for failed validation.
void *rt_workspace_edit_prepare_in_root(void *edits, rt_string root);

/// @brief Prepare an edit batch constrained to multiple workspace roots.
/// @param edits Borrowed Seq of edit maps.
/// @param roots Borrowed Seq of workspace-root strings.
/// @return Owned prepared handle, including for failed validation.
void *rt_workspace_edit_prepare_in_roots(void *edits, void *roots);

/// @brief Test whether a prepared handle is valid and not yet consumed.
int8_t rt_workspace_edit_prepared_is_valid(void *handle);

/// @brief Clone a prepared handle's immutable validation result map.
void *rt_workspace_edit_prepared_result(void *handle);

/// @brief Consume and transactionally apply one prepared edit handle.
/// @return Fresh result map containing validation fields and `appliedFiles`.
void *rt_workspace_edit_prepared_apply(void *handle);

/// @brief Destroy an explicit prepared edit handle; NULL is accepted.
void rt_workspace_edit_prepared_destroy(void *handle);

/// @brief Validate an unrooted workspace edit batch without changing files.
/// @param edits Borrowed Seq of edit maps with file, range, replacement, and optional version
///              fields.
/// @return Fresh map containing `success`, accepted `editCount`, and `diagnostics`.
void *rt_workspace_edit_validate(void *edits);

/// @brief Transactionally apply an unrooted workspace edit batch.
/// @details Stages replacements, rechecks validated content before commit, preserves permissions,
///          and restores backups on a later batch failure when possible.
/// @param edits Borrowed Seq of edit maps.
/// @return Fresh result map containing validation fields and `appliedFiles`.
void *rt_workspace_edit_apply(void *edits);

/// @brief Validate a workspace edit batch against an explicit root directory.
/// @details This is the rooted variant of @ref rt_workspace_edit_validate. Each
///          edit file is canonicalized relative to @p root and rejected if it
///          would escape that tree. The returned map has the same shape as the
///          unrooted validator: `success`, `editCount`, and `diagnostics`.
/// @param edits Seq of edit maps with file/range/newText fields.
/// @param root Workspace root that bounds every edit target.
/// @return Validation result map owned by the caller.
void *rt_workspace_edit_validate_in_root(void *edits, rt_string root);

/// @brief Apply a workspace edit batch constrained to an explicit root directory.
/// @details Validates with @ref rt_workspace_edit_validate_in_root, writes each
///          updated file through a same-directory temporary file, then renames
///          the completed temp into place. Existing files are restored from
///          backups if any later write or rename fails.
/// @param edits Seq of edit maps with file/range/newText fields.
/// @param root Workspace root that bounds every edit target.
/// @return Result map owned by the caller.
void *rt_workspace_edit_apply_in_root(void *edits, rt_string root);

/// @brief Validate a workspace edit batch against explicit workspace roots.
/// @details Every edit target is canonicalized and must be contained by at
///          least one directory in @p roots. Absolute paths are required when
///          more than one root is supplied so relative targets are never
///          resolved ambiguously. The full batch is validated together, which
///          preserves overlap and version checks across root boundaries.
/// @param edits Seq of edit maps with file/range/newText fields.
/// @param roots Seq of workspace-root strings that bound every edit target.
/// @return Validation result map owned by the caller.
void *rt_workspace_edit_validate_in_roots(void *edits, void *roots);

/// @brief Apply one transactional edit batch constrained to workspace roots.
/// @details This is the multi-root counterpart to
///          @ref rt_workspace_edit_apply_in_root. Validation, staging, commit,
///          and rollback cover the complete batch, including edits spanning
///          unrelated workspace folders.
/// @param edits Seq of edit maps with file/range/newText fields.
/// @param roots Seq of workspace-root strings that bound every edit target.
/// @return Result map owned by the caller.
void *rt_workspace_edit_apply_in_roots(void *edits, void *roots);

#ifdef __cplusplus
}
#endif
