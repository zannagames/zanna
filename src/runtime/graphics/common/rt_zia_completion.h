//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_zia_completion.h
/// @brief Declares the runtime bridge to Zia editor and semantic services.
///
/// @details
/// The bridge exposes serialized and structured completion, signature, hover,
/// diagnostics, symbol, semantic-token, document-mirror, project-index, and
/// asynchronous-job APIs through runtime-owned values rather than frontend C++
/// types. Weak fallbacks preserve linkability when editor services are absent.
///
// File: src/runtime/graphics/rt_zia_completion.h
// Purpose: Runtime bridge declarations for the Zia language completion engine, provided by
// zia_editor_services at link time via weak/strong symbol resolution.
//
// Key invariants:
//   - Strong implementations live in src/frontends/zia/rt_zia_completion.cpp
//     (zia_editor_services).
//   - Weak stub implementations in zanna_runtime allow linking without zia_editor_services
//     and return protocol-shaped "completion unavailable" payloads.
//   - Symbols are resolved at final link time when both zia_editor_services and
//     zanna_runtime are linked.
//   - Completion API takes source text, cursor line (1-based), and column (0-based).
//
// Ownership/Lifetime:
//   - Returned completion results are heap-allocated strings; caller must release them.
//   - Source text string is borrowed for the duration of the call only.
//
// Links: src/frontends/zia/rt_zia_completion.cpp (strong implementation),
// src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Run Zia code completion at the given source position.
/// @param source Zia source text (full file contents).
/// @param line   1-based line number of the cursor.
/// @param col    0-based column number of the cursor.
/// @return Tab-delimited completion items: label\tinsertText\tkindInt\tdetail\n
///         Returns an unavailable item when only the weak runtime stub is linked.
rt_string rt_zia_complete(rt_string source, int64_t line, int64_t col);

/// @brief Run Zia code completion with the real source file path for relative bind resolution.
/// @param source    Zia source text (full file contents).
/// @param file_path Absolute or project-relative path for the source file; empty uses "<editor>".
/// @param line      1-based line number of the cursor.
/// @param col       0-based column number of the cursor.
/// @return Tab-delimited completion items: label\tinsertText\tkindInt\tdetail\n
rt_string rt_zia_complete_for_file(rt_string source,
                                   rt_string file_path,
                                   int64_t line,
                                   int64_t col);

/// @brief Run Zia completion and return structured completion maps.
/// @param source Zia source text.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Zanna.Collections.Seq of Zanna.Collections.Map completion items.
void *rt_zia_completion_items(rt_string source, int64_t line, int64_t col);

/// @brief Run path-aware Zia completion and return structured completion maps.
/// @param source Zia source text.
/// @param file_path Source path used for relative resolution.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Owned runtime sequence of completion-item maps.
void *rt_zia_completion_items_for_file(rt_string source,
                                       rt_string file_path,
                                       int64_t line,
                                       int64_t col);

/// @brief Start path-aware completion on a background worker.
/// @param source Zia source text.
/// @param file_path Source path used for relative resolution.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Opaque semantic-job handle.
void *rt_zia_completion_begin_items_for_file(rt_string source,
                                             rt_string file_path,
                                             int64_t line,
                                             int64_t col);

/// @brief Return call signature help for the invocation active at the source location.
/// @param source Zia source text (full file contents).
/// @param line   1-based line number of the cursor.
/// @param col    0-based column number of the cursor.
/// @return Human-readable signature text, or an empty string when no call resolves.
rt_string rt_zia_signature_help(rt_string source, int64_t line, int64_t col);

/// @brief Return call signature help with the real source file path for relative bind resolution.
/// @param source Zia source text.
/// @param file_path Source path used for relative resolution.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Human-readable signature text, or an empty runtime string.
rt_string rt_zia_signature_help_for_file(rt_string source,
                                         rt_string file_path,
                                         int64_t line,
                                         int64_t col);

/// @brief Return structured signature help for the invocation active at the source location.
/// @param source Zia source text.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Owned runtime map describing the active call signature.
void *rt_zia_signature_info(rt_string source, int64_t line, int64_t col);

/// @brief Return structured signature help with the real source file path.
/// @param source Zia source text.
/// @param file_path Source path used for relative resolution.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Owned runtime map describing the active call signature.
void *rt_zia_signature_info_for_file(rt_string source,
                                     rt_string file_path,
                                     int64_t line,
                                     int64_t col);

/// @brief Start path-aware structured signature help on a background worker.
/// @param source Zia source text.
/// @param file_path Source path used for relative resolution.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Opaque semantic-job handle.
void *rt_zia_completion_begin_signature_info_for_file(rt_string source,
                                                      rt_string file_path,
                                                      int64_t line,
                                                      int64_t col);

/// @brief Run semantic analysis and return serialized diagnostics for editor tooling.
/// @param source Zia source text (full file contents).
/// @return One diagnostic per line encoded as severity\tline\tcol\tcode\tmessage.
///         The weak runtime stub returns a warning diagnostic explaining that editor services are
///         absent.
rt_string rt_zia_check(rt_string source);

/// @brief Run semantic analysis with the real source file path for relative bind resolution.
/// @param source Zia source text.
/// @param file_path Source path used in diagnostics and relative binds.
/// @return Serialized diagnostic rows.
rt_string rt_zia_check_for_file(rt_string source, rt_string file_path);

//===----------------------------------------------------------------------===//
// Incremental document mirror sync (plan 08)
//
// The IDE keeps the language service's per-path text mirror current by pushing
// edit deltas instead of the whole buffer on every keystroke. A mirror is keyed
// by @p path and stamped with the editor's monotonic revision. When a delta
// cannot be applied (no baseline mirror, or malformed/gapped deltas) the caller
// falls back to a full sync. Mirror-sourced requests then need no text argument.
//===----------------------------------------------------------------------===//

/// @brief Replace the mirror for @p path with @p text, stamping @p revision.
/// @param path Document path used as the mirror key.
/// @param text Complete replacement text.
/// @param revision Monotonic editor revision assigned to the replacement.
void rt_zia_doc_sync_full(rt_string path, rt_string text, int64_t revision);

/// @brief Apply @p deltas_json (compact edit-delta array) to the mirror for
///        @p path, advancing it to @p end_revision.
/// @param path Document path used as the mirror key.
/// @param deltas_json Serialized compact edit-delta array.
/// @param end_revision Revision assigned after all deltas are applied.
/// @return 1 on success; 0 when there is no baseline mirror or the deltas are
///         malformed (the caller must then issue a full sync).
int8_t rt_zia_doc_sync_delta(rt_string path, rt_string deltas_json, int64_t end_revision);

/// @brief Drop the mirror for @p path (its document was closed).
/// @param path Document path whose mirror is removed.
void rt_zia_doc_close(rt_string path);

/// @brief Return the current mirror text for @p path, or "" when none exists.
/// @details Primarily a testability accessor for byte-exact mirror assertions.
/// @param path Document path used as the mirror key.
/// @return Current mirrored text, or an empty runtime string.
rt_string rt_zia_doc_text(rt_string path);

/// @brief True when a mirror exists for @p path (even if its text is empty).
/// @param path Document path used as the mirror key.
/// @return `1` when a mirror exists; otherwise `0`.
int8_t rt_zia_doc_has(rt_string path);

/// @brief True when the full editor-service bridge is linked (weak stub: 0).
/// @return `1` when strong editor services are available; otherwise `0`.
int8_t rt_zia_service_available(void);

/// @brief Run semantic analysis for @p file_path straight off its mirror text.
/// @param file_path Document path whose mirror is analyzed.
/// @return Diagnostics serialized as severity\tline\tcol\tcode\tmessage per row,
///         or "" when no mirror exists for @p file_path (caller should full-sync).
rt_string rt_zia_check_for_file_mirror(rt_string file_path);

/// @brief Start async structured diagnostics for @p file_path off its mirror
///        text. @return SemanticJobHandle producing Seq<Map> diagnostics, or
///        null when no mirror exists (the caller should full-sync and retry).
/// @param file_path Document path whose mirror is analyzed.
/// @return Opaque semantic-job handle, or `NULL` when no mirror exists.
void *rt_zia_doc_begin_check_for_file(rt_string file_path);

/// @brief Run semantic analysis and return structured diagnostic maps.
/// @param source Zia source text.
/// @return Zanna.Collections.Seq of Zanna.Collections.Map diagnostics.
void *rt_zia_toolchain_check(rt_string source);

/// @brief Run path-aware semantic analysis and return structured diagnostic maps.
/// @param source Zia source text.
/// @param file_path Source path used in diagnostics and relative binds.
/// @return Owned runtime sequence of diagnostic maps.
void *rt_zia_toolchain_check_for_file(rt_string source, rt_string file_path);

/// @brief Start path-aware semantic diagnostics on a background worker.
/// @param source Zia source text.
/// @param file_path Source path used in diagnostics and relative binds.
/// @return Opaque semantic-job handle.
void *rt_zia_toolchain_begin_check_for_file(rt_string source, rt_string file_path);

/// @brief Compile source to IL and return a structured result map.
/// @param source Zia source text.
/// @return Zanna.Collections.Map with success, diagnostics, sourcePath, outputPath, and il.
void *rt_zia_toolchain_compile(rt_string source);

/// @brief Compile source with a file path and return a structured result map.
/// @param source Zia source text.
/// @param file_path Source path used for diagnostics and output naming.
/// @return Owned runtime compile-result map.
void *rt_zia_toolchain_compile_for_file(rt_string source, rt_string file_path);

/// @brief Create a project language index rooted at @p root.
/// @param root Project root directory.
/// @return Opaque Zanna.Zia.ProjectIndex.ProjectIndexHandle object, or null when editor services
/// are
///         unavailable.
void *rt_zia_project_index_new(rt_string root);

/// @brief Check whether @p handle is a live ProjectIndex handle.
/// @param handle Candidate project-index handle.
/// @return `1` when valid; otherwise `0`.
int8_t rt_zia_project_index_is_valid(void *handle);

/// @brief Store dirty/current source for @p file_path in the project index.
/// @param handle Project-index handle.
/// @param file_path File path used as the index key.
/// @param source Current source text.
/// @return `1` when the index was updated; otherwise `0`.
int8_t rt_zia_project_index_update_file(void *handle, rt_string file_path, rt_string source);

/// @brief Remove @p file_path from the project index.
/// @param handle Project-index handle.
/// @param file_path File path to remove.
/// @return `1` when an indexed file was removed; otherwise `0`.
int8_t rt_zia_project_index_remove_file(void *handle, rt_string file_path);

/// @brief Remove all files from the project index.
/// @param handle Project-index handle.
void rt_zia_project_index_clear(void *handle);

/// @brief Dispose the native project index payload. The handle object remains inert.
/// @param handle Project-index handle to invalidate.
void rt_zia_project_index_destroy(void *handle);

/// @brief Return a structured definition map for the identifier at @p line/@p col.
/// @param handle Project-index handle.
/// @param file_path Current source file path.
/// @param source Current source text.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Owned runtime definition map.
void *rt_zia_project_index_definition(
    void *handle, rt_string file_path, rt_string source, int64_t line, int64_t col);

/// @brief Return structured semantic references for the identifier at @p line/@p col.
/// @param handle Project-index handle.
/// @param file_path Current source file path.
/// @param source Current source text.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Owned runtime sequence of reference maps.
void *rt_zia_project_index_references(
    void *handle, rt_string file_path, rt_string source, int64_t line, int64_t col);

/// @brief Return workspace edits for a semantic rename without applying them.
/// @param handle Project-index handle.
/// @param file_path Current source file path.
/// @param source Current source text.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @param new_name Replacement identifier.
/// @return Owned runtime workspace-edit collection.
void *rt_zia_project_index_rename_edits(void *handle,
                                        rt_string file_path,
                                        rt_string source,
                                        int64_t line,
                                        int64_t col,
                                        rt_string new_name);

/// @brief Return hover information for the identifier at the given source location.
/// @param source Zia source text (full file contents).
/// @param line   1-based line number of the cursor.
/// @param col    0-based column number of the cursor.
/// @return Human-readable hover text, or an empty string when nothing is found.
rt_string rt_zia_hover(rt_string source, int64_t line, int64_t col);

/// @brief Return hover information with the real source file path for relative bind resolution.
/// @param source Zia source text.
/// @param file_path Source path used for relative resolution.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Human-readable hover text, or an empty runtime string.
rt_string rt_zia_hover_for_file(rt_string source, rt_string file_path, int64_t line, int64_t col);

/// @brief Return structured hover information for the identifier at the given source location.
/// @param source Zia source text.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Owned runtime hover-information map.
void *rt_zia_hover_info(rt_string source, int64_t line, int64_t col);

/// @brief Return structured hover information with the real source file path.
/// @param source Zia source text.
/// @param file_path Source path used for relative resolution.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Owned runtime hover-information map.
void *rt_zia_hover_info_for_file(rt_string source, rt_string file_path, int64_t line, int64_t col);

/// @brief Start path-aware structured hover info on a background worker.
/// @param source Zia source text.
/// @param file_path Source path used for relative resolution.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Opaque semantic-job handle.
void *rt_zia_completion_begin_hover_info_for_file(rt_string source,
                                                  rt_string file_path,
                                                  int64_t line,
                                                  int64_t col);

/// @brief Return serialized document symbols for the supplied source.
/// @param source Zia source text (full file contents).
/// @return Tab-delimited symbol rows, or an empty string when no symbols are found.
rt_string rt_zia_symbols(rt_string source);

/// @brief Return document symbols with the real source file path for relative bind resolution.
/// @param source Zia source text.
/// @param file_path Source path used for relative resolution.
/// @return Serialized document-symbol rows.
rt_string rt_zia_symbols_for_file(rt_string source, rt_string file_path);

/// @brief Start path-aware document symbol extraction on a background worker.
/// @param source Zia source text.
/// @param file_path Source path used for relative resolution.
/// @return Opaque semantic-job handle.
void *rt_zia_completion_begin_symbols_for_file(rt_string source, rt_string file_path);

/// @brief Start path-aware semantic-token classification on a background worker.
/// @param source Zia source text.
/// @param file_path Source path used for relative resolution.
/// @return Opaque semantic-job handle.
void *rt_zia_completion_begin_tokens_for_file(rt_string source, rt_string file_path);

/// @brief Start one path-aware analysis producing symbols, semantic tokens, and diagnostics.
/// @details The compiler parses and binds the supplied snapshot once. The completed job supports
///          `SemanticJob.Symbols`, `SemanticJob.Tokens`, and `SemanticJob.Diagnostics`.
/// @param source Zia source text.
/// @param file_path Source path used for relative resolution and diagnostics.
/// @return Opaque semantic-job handle.
void *rt_zia_completion_begin_analysis_for_file(rt_string source, rt_string file_path);

/// @brief Return whether a semantic background job has completed.
/// @param handle Semantic-job handle.
/// @return `1` when the job is complete; otherwise `0`.
int8_t rt_zia_semantic_job_is_done(void *handle);

/// @brief Return whether a completed semantic background job failed.
/// @param handle Semantic-job handle.
/// @return `1` when the completed job failed; otherwise `0`.
int8_t rt_zia_semantic_job_is_error(void *handle);

/// @brief Return a completed semantic background job's error string, if any.
/// @param handle Semantic-job handle.
/// @return Error text, or an empty runtime string.
rt_string rt_zia_semantic_job_error(void *handle);

/// @brief Return a semantic background job's error as an Option string.
/// @details Returns `SomeStr(message)` when the job has a non-empty error
///          payload, otherwise `None`. This is the preferred API for new code
///          because absence is explicit and cannot be confused with an empty
///          error string.
/// @param handle Semantic-job handle.
/// @return Runtime Option containing the error string when present.
void *rt_zia_semantic_job_error_option(void *handle);

/// @brief Return the numeric semantic background job kind.
/// @param handle Semantic-job handle.
/// @return Numeric job-kind identifier.
int64_t rt_zia_semantic_job_kind(void *handle);

/// @brief Mark a semantic background job as canceled. Running work may finish later.
/// @param handle Semantic-job handle.
void rt_zia_semantic_job_cancel(void *handle);

/// @brief Materialize a completion job result as Seq<Map>.
/// @param handle Completed completion-job handle.
/// @return Owned runtime sequence of completion maps.
void *rt_zia_semantic_job_completion_items(void *handle);

/// @brief Materialize a signature job result as Map.
/// @param handle Completed signature-job handle.
/// @return Owned runtime signature-information map.
void *rt_zia_semantic_job_signature_info(void *handle);

/// @brief Materialize a hover job result as Map.
/// @param handle Completed hover-job handle.
/// @return Owned runtime hover-information map.
void *rt_zia_semantic_job_hover_info(void *handle);

/// @brief Materialize a symbols job result as serialized symbol rows.
/// @param handle Completed symbols-job handle.
/// @return Serialized symbol rows.
rt_string rt_zia_semantic_job_symbols(void *handle);

/// @brief Materialize a tokens job result as serialized semantic-token rows.
/// @param handle Completed semantic-token-job handle.
/// @return Serialized semantic-token rows.
rt_string rt_zia_semantic_job_tokens(void *handle);

/// @brief Materialize a diagnostics job result as Seq<Map>.
/// @param handle Completed diagnostics-job handle.
/// @return Owned runtime sequence of diagnostic maps.
void *rt_zia_semantic_job_diagnostics(void *handle);

/// @brief Flush the cached parse result, forcing a fresh parse on the next call.
void rt_zia_completion_clear_cache(void);

#ifdef __cplusplus
}
#endif
