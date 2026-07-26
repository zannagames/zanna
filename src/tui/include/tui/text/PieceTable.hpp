//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares append-only piece-table text storage for the TUI editor.
/// @details The logical document is a list of spans into immutable original and
///          append-only add buffers. Mutations return owning Change metadata for
///          undo/redo while preserving existing buffer contents.
//
// Insertions append new text to the add buffer and split/insert pieces.
// Deletions split pieces and remove the deleted range. Neither operation
// modifies existing buffer content, making the piece table inherently
// suited for undo/redo via Change objects that capture span metadata.
//
// The Change struct returned from mutating operations records what was
// inserted and/or erased, enabling the EditHistory to replay operations
// in both directions for undo and redo.
//
// Key invariants:
//   - The original buffer is never modified after load().
//   - The add buffer is append-only; text is never removed from it.
//   - The sum of all piece lengths equals size().
//   - Piece boundaries are always consistent after each operation.
//
// Ownership: PieceTable owns both buffers and the piece list by value.
// Change objects returned from mutations own copies of affected text.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace zanna::tui::text {
/// @brief Piece table implementation providing efficient insert/erase operations
///        for text editing in the TUI editor.
/// @details Uses an original buffer (set at load time, never modified) and an
///          append-only add buffer. The document is represented as an ordered list
///          of pieces, each referencing a contiguous span in one of the two buffers.
///          This structure provides O(n) insert/erase where n is the number of pieces,
///          which remains small for typical editing sessions.
class PieceTable {
  public:
    /// @brief Captures the metadata of a single piece table mutation for undo/redo.
    /// @details Records both the inserted and erased spans (position + text) from a
    ///          single insert or erase operation. The EditHistory stores these changes
    ///          and replays them in reverse for undo or forward for redo.
    struct Change {
        /// @brief Callback signature receiving span position and text view.
        /// @param pos Logical byte offset of the changed span.
        /// @param text Borrowed inserted or erased bytes.
        using Callback = std::function<void(std::size_t pos, std::string_view text)>;

        /// @brief Record inserted span metadata and payload.
        /// @param pos Logical byte offset of the insertion.
        /// @param text Owning copy of the inserted bytes.
        void recordInsert(std::size_t pos, std::string text);

        /// @brief Record erased span metadata and payload.
        /// @param pos Logical byte offset of the erasure.
        /// @param text Owning copy of the erased bytes.
        void recordErase(std::size_t pos, std::string text);

        /// @brief Notify listener about inserted span, if any.
        /// @param cb Callback invoked with the insertion position and borrowed text.
        void notifyInsert(const Callback &cb) const;

        /// @brief Notify listener about erased span, if any.
        /// @param cb Callback invoked with the erasure position and borrowed text.
        void notifyErase(const Callback &cb) const;

        /// @brief True if an insert span is present.
        /// @return true after recordInsert() has stored a span.
        [[nodiscard]] bool hasInsert() const;

        /// @brief True if an erase span is present.
        /// @return true after recordErase() has stored a span.
        [[nodiscard]] bool hasErase() const;

        /// @brief Position of inserted span (undefined if !hasInsert()).
        /// @return Recorded logical insertion byte offset.
        [[nodiscard]] std::size_t insertPos() const;

        /// @brief Position of erased span (undefined if !hasErase()).
        /// @return Recorded logical erasure byte offset.
        [[nodiscard]] std::size_t erasePos() const;

        /// @brief Inserted text view (empty if !hasInsert()).
        /// @return View valid for the lifetime of this Change or until it is modified.
        [[nodiscard]] std::string_view insertedText() const;

        /// @brief Erased text view (empty if !hasErase()).
        /// @return View valid for the lifetime of this Change or until it is modified.
        [[nodiscard]] std::string_view erasedText() const;

      private:
        /// @brief Owned position and text payload for one mutation direction.
        struct Span {
            std::size_t pos{}; ///< Logical byte offset.
            std::string text{}; ///< Inserted or erased bytes.
        };

        std::optional<Span> insert_span_{}; ///< Recorded insertion, when present.
        std::optional<Span> erase_span_{};  ///< Recorded erasure, when present.
    };

    /// @brief Replace all content with fresh text, resetting both buffers.
    /// @details Clears the piece list, sets the original buffer to the given text,
    ///          empties the add buffer, and creates a single piece spanning the
    ///          entire original buffer. Returns a Change for undo tracking.
    /// @param text New content to load.
    /// @return Change describing the full replacement.
    Change load(std::string text);

    /// @brief Current byte size.
    /// @return Total logical document size in bytes.
    [[nodiscard]] std::size_t size() const;

    /// @brief Extract a substring from the logical document.
    /// @details Walks the piece list to find pieces overlapping [pos, pos+len)
    ///          and copies the relevant byte ranges into a contiguous string.
    /// @param pos Starting byte offset in the logical document.
    /// @param len Number of bytes to extract.
    /// @return The extracted text as a contiguous string.
    [[nodiscard]] std::string getText(std::size_t pos, std::size_t len) const;

    /// @brief Visit contiguous buffer segments covering a byte range without copying.
    /// @details Iterates the piece list and invokes the visitor for each contiguous
    ///          string_view segment within [pos, pos+len). The visitor returns false
    ///          to stop early. This avoids string allocation for rendering.
    /// @tparam Fn Callable taking std::string_view, returning bool.
    /// @param pos Starting byte offset.
    /// @param len Number of bytes in the range.
    /// @param fn Visitor invoked for each segment.
    template <typename Fn> void forEachSegment(std::size_t pos, std::size_t len, Fn &&fn) const;

    /// @brief Insert text at the given byte position within the logical document.
    /// @details Appends the new text to the add buffer, then splits the piece list
    ///          at the insertion point and inserts a new piece referencing the
    ///          appended text. Returns a Change recording the insertion metadata.
    /// @param pos Byte offset where text will be inserted.
    /// @param text The text to insert.
    /// @return Change describing the insertion for undo/redo tracking.
    Change insertInternal(std::size_t pos, std::string_view text);

    /// @brief Erase a range of bytes from the logical document.
    /// @details Splits pieces at the erase boundaries and removes all pieces
    ///          (or partial pieces) within the erased range. The erased text is
    ///          captured in the returned Change for undo replay. Neither buffer
    ///          is modified; only the piece list is updated.
    /// @param pos Starting byte offset of the range to erase.
    /// @param len Number of bytes to erase.
    /// @return Change describing the erasure for undo/redo tracking.
    Change eraseInternal(std::size_t pos, std::size_t len);

  private:
    /// @brief Physical buffer referenced by a piece.
    enum class BufferKind { Original, Add };

    /// @brief One contiguous logical-document span into a physical buffer.
    struct Piece {
        BufferKind buf{};   ///< Source buffer containing the bytes.
        std::size_t start{}; ///< Byte offset within the source buffer.
        std::size_t length{}; ///< Number of logical bytes in this piece.
    };

    /// @brief Locate the piece containing or following a logical byte offset.
    /// @param pos Logical document byte offset.
    /// @param offset Output offset within the returned piece.
    /// @return Mutable iterator to the containing/following piece or end().
    std::list<Piece>::iterator findPiece(std::size_t pos, std::size_t &offset);

    /// @brief Locate the piece containing or following a logical byte offset.
    /// @param pos Logical document byte offset.
    /// @param offset Output offset within the returned piece.
    /// @return Const iterator to the containing/following piece or end().
    std::list<Piece>::const_iterator findPiece(std::size_t pos, std::size_t &offset) const;

    std::list<Piece> pieces_{}; ///< Ordered spans forming the logical document.
    std::string original_{};    ///< Immutable-after-load original bytes.
    std::string add_{};         ///< Append-only bytes introduced by insertions.
    std::size_t size_{};        ///< Cached sum of all piece lengths.
};
} // namespace zanna::tui::text

/// @brief Visit physical segments covering a logical document range.
/// @tparam Fn Callable accepting a string view and returning whether iteration continues.
/// @param pos Starting logical byte offset.
/// @param len Maximum number of logical bytes to visit.
/// @param fn Visitor receiving each contiguous borrowed segment.
template <typename Fn>
void zanna::tui::text::PieceTable::forEachSegment(std::size_t pos, std::size_t len, Fn &&fn) const {
    std::size_t idx = 0;
    for (auto it = pieces_.cbegin(); it != pieces_.cend() && len > 0; ++it) {
        if (pos >= idx + it->length) {
            idx += it->length;
            continue;
        }

        std::size_t start_in_piece = pos > idx ? pos - idx : 0U;
        std::size_t take = (std::min)(it->length - start_in_piece, len);
        const std::string &buf = it->buf == BufferKind::Add ? add_ : original_;
        std::string_view view(buf.data() + it->start + start_in_piece, take);
        if (!std::invoke(std::forward<Fn>(fn), view)) {
            break;
        }

        pos += take;
        len -= take;
        idx += it->length;
    }
}
