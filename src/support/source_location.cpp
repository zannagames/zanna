//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Provides the minimal out-of-line utilities for the SourceLoc value type.  A
// location is considered valid when it refers to a registered file identifier;
// line and column components are optional and surfaced through `hasLine()` and
// `hasColumn()` respectively.  Keeping the helper here avoids inlining the
// check into every translation unit that includes the header while preserving a
// central explanation of the validity contract.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements validity queries for `SourceLoc` and `SourceRange`.
/// @details The majority of `SourceLoc` operations are inline, but the validity
///          predicates live out-of-line to centralise their documentation and
///          keep translation units lightweight.  These helpers are heavily used
///          when printing diagnostics or deciding whether to attach source
///          locations to serialized IL entities.

#include "support/source_location.hpp"

namespace il::support {
/// @brief Determine whether the location carries a real source attachment.
///
/// @details SourceManager dispenses monotonically increasing identifiers for
///          every file registered with the compiler.  The default-constructed
///          location uses zero to mark "unknown".  By testing the stored
///          identifier against zero, the helper distinguishes between genuine,
///          user-authored locations and synthesized values, enabling
///          diagnostics and serializers to elide missing information.
///
/// @return True when the location originated from a tracked source file.
bool SourceLoc::isValid() const {
    return file_id != 0;
}

/// @brief Determine whether the range has complete usable source coordinates.
///
/// @details Valid ranges are either concrete replacement spans or explicit
///          zero-width insertion points.  This keeps the public predicate aligned
///          with the range invariant while preserving the older permissive query
///          through @ref isTracked.
///
/// @return True when the range is concrete or an insertion point.
bool SourceRange::isValid() const {
    return isConcrete() || isInsertion();
}

/// @brief Determine whether the range endpoints refer to tracked source.
///
/// @details This helper intentionally accepts partially-known coordinates as long
///          as both endpoints name the same file.  When line and column metadata
///          is present it rejects reversed order, while allowing zero-width
///          insertion points.
///
/// @return True when both endpoints carry compatible valid file ids.
bool SourceRange::isTracked() const {
    if (!begin.isValid() || !end.isValid()) {
        return false;
    }

    if (begin.file_id != end.file_id) {
        return false;
    }

    const bool have_line_info = begin.line != 0 && end.line != 0;
    if (have_line_info && begin.line > end.line) {
        return false;
    }

    const bool have_column_info =
        have_line_info && begin.line == end.line && begin.column != 0 && end.column != 0;
    if (have_column_info && begin.column > end.column) {
        return false;
    }

    return true;
}

/// @brief Determine whether the range is a fully-addressed replacement span.
///
/// @details Unlike the insertion case also accepted by @ref isValid, a concrete
///          range must have nonzero width. Both endpoints must carry file, line,
///          and column information, must refer to the same file, and the begin
///          point must strictly precede the end point. This prevents partially
///          known ranges from being serialized as replacement spans.
///
/// @return True when the range has complete coordinates and nonzero length.
bool SourceRange::isConcrete() const {
    if (!begin.hasFile() || !end.hasFile() || begin.file_id != end.file_id)
        return false;
    if (!begin.hasLine() || !end.hasLine() || !begin.hasColumn() || !end.hasColumn())
        return false;
    if (begin.line < end.line)
        return true;
    if (begin.line > end.line)
        return false;
    return begin.column < end.column;
}

/// @brief Determine whether the range encodes an insertion point.
///
/// @details A zero-width range is not a replacement span, but callers can use this
///          helper to recognize explicit insertion coordinates instead of relying
///          on invalid or partially-known ranges.
///
/// @return True when both endpoints have identical concrete coordinates.
bool SourceRange::isInsertion() const {
    return begin.hasFile() && end.hasFile() && begin.file_id == end.file_id && begin.hasLine() &&
           end.hasLine() && begin.line == end.line && begin.hasColumn() && end.hasColumn() &&
           begin.column == end.column;
}
} // namespace il::support
