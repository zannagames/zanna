//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tui/src/text/text_buffer.cpp
// Purpose: Coordinate the piece table, line index, and edit history that power
//          the terminal UI text editor.
// Key invariants: The helper subsystems stay synchronised through change
//                 callbacks; all user-facing strings are copied from the
//                 underlying storage so callers never observe dangling views.
// Ownership/Lifetime: TextBuffer owns its @ref PieceTable, @ref LineIndex, and
//                     @ref EditHistory instances outright and therefore manages
//                     their lifetimes as a unit.
// Links: docs/internals/architecture.md#zannatui-architecture
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implementation of the @ref zanna::tui::text::TextBuffer façade.
/// @details `TextBuffer` bundles the editable storage (piece table), indexing
///          metadata (line index), and undo/redo machinery (edit history).  Each
///          mutating operation updates all three structures through explicit
///          callbacks so the buffer remains internally consistent even when
///          complex edit sequences are replayed.

#include "tui/text/text_buffer.hpp"

#include <utility>

namespace zanna::tui::text {
/// @brief Create a lightweight view over a contiguous span within the piece table.
/// @details Stores the referenced table along with the byte offset and length of
///          the slice.  Views provide a convenience wrapper so callers can visit
///          the contributing segments without exposing the piece-table internals.
/// @param table Underlying storage whose contents are being viewed.
/// @param offset Starting byte offset within @p table.
/// @param length Number of bytes covered by the view.
TextBuffer::LineView::LineView(const PieceTable &table, std::size_t offset, std::size_t length)
    : table_(table), offset_(offset), length_(length) {}

/// @brief Retrieve the starting byte offset of the view.
/// @return Offset originally supplied to the constructor.
std::size_t TextBuffer::LineView::offset() const {
    return offset_;
}

/// @brief Retrieve the length of the viewed range in bytes.
/// @return Number of bytes covered by the view.
std::size_t TextBuffer::LineView::length() const {
    return length_;
}

/// @brief Visit every contiguous segment that composes the view.
/// @details Delegates to @ref PieceTable::forEachSegment so callers receive each
///          contributing span in document order.  This is primarily used by the
///          renderer when painting wrapped lines.
/// @param fn Callback invoked with segment descriptors.
void TextBuffer::LineView::forEachSegment(SegmentVisitor fn) const {
    table_.forEachSegment(offset_, length_, fn);
}

/// @brief Replace the buffer contents with @p text and reset helper state.
/// @details Delegates to @ref PieceTable::load, rebuilds the @ref LineIndex, and
///          clears the undo history so subsequent edits start from a clean slate.
/// @param text New document contents.
void TextBuffer::load(std::string text) {
    auto change = table_.load(std::move(text));
    line_index_.reset(change.insertedText());
    history_.clear();
}

/// @brief Obtain the size of the document measured in bytes.
/// @return Total number of bytes represented by the piece table.
std::size_t TextBuffer::size() const {
    return table_.size();
}

/// @brief Report how many logical lines the buffer currently contains.
/// @details The count is sourced from @ref LineIndex and therefore reflects any
///          newline-aware transformations applied by recent edits.
/// @return Number of tracked line starts.
std::size_t TextBuffer::lineCount() const {
    return line_index_.count();
}

/// @brief Resolve the byte offset where a given line begins.
/// @details Bounds-checks the line number and returns the buffer end when the
///          request falls beyond the known range.  This mirrors how text editors
///          expose sentinel positions for out-of-range queries.
/// @param lineNo Zero-based line index.
/// @return Byte offset of the first character in the requested line.
std::size_t TextBuffer::lineStart(std::size_t lineNo) const {
    if (lineNo >= line_index_.count()) {
        return table_.size();
    }
    return line_index_.start(lineNo);
}

/// @brief Resolve the byte offset immediately after a line's visible contents.
/// @details For non-final lines, subtracts the terminating newline byte from the
///          next line's start.  The final line ends at the logical buffer size,
///          and an out-of-range line number likewise resolves to that sentinel.
/// @param lineNo Zero-based line index.
/// @return Exclusive end offset of the line contents, excluding a terminating
///         newline when one is present.
std::size_t TextBuffer::lineEnd(std::size_t lineNo) const {
    if (lineNo >= line_index_.count()) {
        return table_.size();
    }

    const std::size_t start = line_index_.start(lineNo);
    const std::size_t nextLine = lineNo + 1;
    if (nextLine < line_index_.count()) {
        const std::size_t nextStart = line_index_.start(nextLine);
        if (nextStart > start) {
            return nextStart - 1;
        }
        return start;
    }
    return table_.size();
}

/// @brief Convenience alias for @ref lineStart.
/// @param lineNo Zero-based line index.
/// @return Byte offset where the line begins.
std::size_t TextBuffer::lineOffset(std::size_t lineNo) const {
    return lineStart(lineNo);
}

/// @brief Measure the number of bytes occupied by a line.
/// @details Computes the difference between the start and end offsets, guarding
///          against pathological ranges where the end precedes the start.
/// @param lineNo Zero-based line index.
/// @return Length of the line in bytes (excluding the trailing newline).
std::size_t TextBuffer::lineLength(std::size_t lineNo) const {
    const std::size_t start = lineStart(lineNo);
    const std::size_t end = lineEnd(lineNo);
    if (end <= start) {
        return 0;
    }
    return end - start;
}

/// @brief Materialise a @ref LineView for the requested line.
/// @param lineNo Zero-based line index.
/// @return Lightweight view describing the selected line's byte range.
TextBuffer::LineView TextBuffer::lineView(std::size_t lineNo) const {
    return LineView(table_, lineOffset(lineNo), lineLength(lineNo));
}

/// @brief Begin an undo transaction.
/// @details Forwards to @ref EditHistory::beginTxn so multiple edits can be
///          coalesced into a single undo step.
void TextBuffer::beginTxn() {
    history_.beginTxn();
}

/// @brief Complete the current undo transaction.
/// @details Mirrors @ref beginTxn by signalling the edit history that a batch of
///          operations is ready to be committed.
void TextBuffer::endTxn() {
    history_.endTxn();
}

/// @brief Insert text at the specified byte offset.
/// @details Applies the mutation to the piece table, forwards change callbacks to
///          the line index, and records an undo entry when text was actually
///          inserted.  This keeps all helper structures in sync.
/// @param pos Byte offset where the insertion occurs.
/// @param text UTF-8 string to insert.
void TextBuffer::insert(std::size_t pos, std::string_view text) {
    auto change = table_.insertInternal(pos, text);
    /// @brief Forward one piece-table insertion to the line index.
    /// @param changePos Byte offset of the applied insertion.
    /// @param inserted Inserted UTF-8 bytes.
    change.notifyInsert([this](std::size_t changePos, std::string_view inserted) {
        line_index_.onInsert(changePos, inserted);
    });
    if (change.hasInsert()) {
        history_.recordInsert(change.insertPos(), std::string(change.insertedText()));
    }
}

/// @brief Erase a span of bytes from the buffer.
/// @details Applies the removal to the piece table, notifies the line index, and
///          records an undo entry containing the removed text.  No-op removals are
///          ignored so the history remains concise.
/// @param pos Starting byte offset to erase.
/// @param len Number of bytes to remove.
void TextBuffer::erase(std::size_t pos, std::size_t len) {
    auto change = table_.eraseInternal(pos, len);
    /// @brief Forward one piece-table erasure to the line index.
    /// @param changePos Byte offset of the applied erasure.
    /// @param removed Removed UTF-8 bytes.
    change.notifyErase([this](std::size_t changePos, std::string_view removed) {
        line_index_.onErase(changePos, removed);
    });
    if (change.hasErase()) {
        history_.recordErase(change.erasePos(), std::string(change.erasedText()));
    }
}

/// @brief Undo the most recent edit, if any.
/// @details Delegates to @ref EditHistory::undo while replaying the recorded
///          operations back into the piece table.  The lambda mirrors the logic of
///          @ref insert and @ref erase so auxiliary structures observe identical
///          callbacks during undo.
/// @return True when an edit was undone.
bool TextBuffer::undo() {
    /// @brief Replay one history operation in reverse against the piece table.
    /// @param op Recorded edit operation to undo.
    return history_.undo([this](const EditHistory::Op &op) {
        if (op.type == EditHistory::OpType::Insert) {
            auto change = table_.eraseInternal(op.pos, op.text.size());
            /// @brief Forward an undo erasure to the line index.
            /// @param changePos Byte offset of the erasure.
            /// @param removed Removed UTF-8 bytes.
            change.notifyErase([this](std::size_t changePos, std::string_view removed) {
                line_index_.onErase(changePos, removed);
            });
        } else {
            auto change = table_.insertInternal(op.pos, op.text);
            /// @brief Forward an undo insertion to the line index.
            /// @param changePos Byte offset of the insertion.
            /// @param inserted Inserted UTF-8 bytes.
            change.notifyInsert([this](std::size_t changePos, std::string_view inserted) {
                line_index_.onInsert(changePos, inserted);
            });
        }
    });
}

/// @brief Redo the most recently undone edit.
/// @details Mirrors @ref undo but replays the redo log in the forward direction.
///          The same callback structure updates the piece table, line index, and
///          edit history in lockstep.
/// @return True when an edit was reapplied.
bool TextBuffer::redo() {
    /// @brief Replay one history operation forward against the piece table.
    /// @param op Recorded edit operation to redo.
    return history_.redo([this](const EditHistory::Op &op) {
        if (op.type == EditHistory::OpType::Insert) {
            auto change = table_.insertInternal(op.pos, op.text);
            /// @brief Forward a redo insertion to the line index.
            /// @param changePos Byte offset of the insertion.
            /// @param inserted Inserted UTF-8 bytes.
            change.notifyInsert([this](std::size_t changePos, std::string_view inserted) {
                line_index_.onInsert(changePos, inserted);
            });
        } else {
            auto change = table_.eraseInternal(op.pos, op.text.size());
            /// @brief Forward a redo erasure to the line index.
            /// @param changePos Byte offset of the erasure.
            /// @param removed Removed UTF-8 bytes.
            change.notifyErase([this](std::size_t changePos, std::string_view removed) {
                line_index_.onErase(changePos, removed);
            });
        }
    });
}

/// @brief Materialise the entire buffer as a single string.
/// @details Forwards to @ref PieceTable::getText using the document bounds.
/// @return Copy of the buffer contents.
std::string TextBuffer::str() const {
    return table_.getText(0, table_.size());
}

/// @brief Retrieve a single line of text without its trailing newline.
/// @details Uses @ref LineIndex to compute the start/end offsets and copies the
///          range out of the piece table.  Out-of-range requests yield an empty
///          string, matching editor expectations.
/// @param lineNo Zero-based line index.
/// @return Copy of the requested line.
std::string TextBuffer::getLine(std::size_t lineNo) const {
    if (lineNo >= line_index_.count()) {
        return {};
    }
    std::size_t start = line_index_.start(lineNo);
    std::size_t end =
        (lineNo + 1 < line_index_.count()) ? line_index_.start(lineNo + 1) - 1 : table_.size();
    if (end < start) {
        end = start;
    }
    return table_.getText(start, end - start);
}

/// @brief Visit each line in the buffer with a lazily constructed view.
/// @details Iterates over the cached line index and invokes @p fn with a
///          @ref LineView that references, but does not copy, the underlying
///          text.  The visitor may terminate early by returning @c false.
/// @param fn Callback invoked for every line; returns @c false to stop early.
void TextBuffer::forEachLine(LineVisitor fn) const {
    const std::size_t lines = line_index_.count();
    for (std::size_t line = 0; line < lines; ++line) {
        LineView view(table_, lineOffset(line), lineLength(line));
        if (!fn(line, view)) {
            break;
        }
    }
}
} // namespace zanna::tui::text
