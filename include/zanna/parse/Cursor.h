//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/parse/Cursor.h
// Purpose: Declare a lightweight text cursor for IL parsers.
// Key invariants: Operates on a string_view without allocating or owning storage.
// Ownership/Lifetime: Views textual buffers owned by the caller; no allocations.
// Links: docs/il/il-guide.md#reference, src/parse/Cursor.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/parse/Cursor.h
 * @brief Defines a non-owning, position-aware cursor for IL text parsing.
 *
 * @details
 * `Cursor` provides zero-allocation scanning primitives shared by the function
 * and operand parsers. It tracks both an absolute byte offset and a caller-
 * supplied line/column origin while consuming whitespace, identifiers, signed
 * integers, and keywords.
 *
 * Every returned `string_view` aliases the original source buffer. The buffer
 * must therefore remain alive and at a stable address for the cursor and all
 * views derived from it.
 */

#pragma once

#include <concepts>
#include <cstddef>
#include <string_view>

namespace zanna::parse
{

/**
 * @brief Constrains predicates accepted by `Cursor::consumeWhile`.
 *
 * @tparam Predicate Callable type tested against a single character.
 *
 * A conforming predicate can be invoked with `char` and returns a type
 * convertible to `bool`. The cursor does not retain the predicate.
 */
template <class Predicate>
concept CursorPredicate = requires(Predicate pred, char ch) {
    { pred(ch) } -> std::convertible_to<bool>;
};

/**
 * @brief Identifies a line and byte-column position in a textual buffer.
 *
 * The parser convention uses one-based line numbers and zero-based columns.
 * The all-zero default can represent an unspecified position; a cursor
 * otherwise preserves the coordinate basis supplied by its caller.
 */
struct SourcePos
{
    unsigned line = 0;      ///< One-based line number, or zero when unspecified.
    std::size_t column = 0; ///< Zero-based byte offset within the current line.
};

/**
 * @brief Scans caller-owned text while maintaining byte and source positions.
 *
 * @invariant `offset()` never exceeds `view().size()`.
 * @invariant `pos()` describes the location reached by consuming the prefix
 *            `[0, offset())` from the constructor's starting position.
 *
 * @ownership The cursor borrows its source character storage. Copying a cursor
 *            copies only the view and current scan state.
 */
class Cursor
{
  public:
    /**
     * @brief Construct a cursor at the beginning of a source view.
     *
     * @param text Borrowed source buffer to scan.
     * @param start Line and column assigned to byte offset zero.
     *
     * @pre The storage referenced by `text` outlives the cursor and every view
     *      returned from it.
     * @post `offset()` is zero and `pos()` equals `start`.
     */
    Cursor(std::string_view text, SourcePos start) noexcept;

    /**
     * @brief Return the complete source view supplied at construction.
     * @return A borrowed view of the full buffer, independent of the current
     *         offset.
     */
    [[nodiscard]] std::string_view view() const noexcept
    {
        return text_;
    }

    /**
     * @brief View the unconsumed suffix.
     * @return A borrowed view spanning `[offset(), view().size())`.
     */
    [[nodiscard]] std::string_view remaining() const noexcept
    {
        return text_.substr(index_);
    }

    /**
     * @brief Query whether the cursor has reached the end of the buffer.
     * @return `true` when no characters remain to be consumed.
     */
    [[nodiscard]] bool atEnd() const noexcept
    {
        return index_ >= text_.size();
    }

    /**
     * @brief Inspect the current character without changing cursor state.
     * @return The byte at `offset()`, or the sentinel `'\0'` at end of input.
     *
     * @note The sentinel is indistinguishable from an embedded NUL byte unless
     *       the caller also checks `atEnd()`.
     */
    [[nodiscard]] char peek() const noexcept;

    /**
     * @brief Report the source location corresponding to the current byte.
     * @return The tracked line and column without changing cursor state.
     */
    [[nodiscard]] SourcePos pos() const noexcept
    {
        return pos_;
    }

    /**
     * @brief Retrieve the caller-based current line coordinate.
     * @return The line component of `pos()`.
     */
    [[nodiscard]] unsigned line() const noexcept
    {
        return pos_.line;
    }

    /**
     * @brief Retrieve the current byte-column coordinate.
     * @return The column component of `pos()`.
     */
    [[nodiscard]] std::size_t column() const noexcept
    {
        return pos_.column;
    }

    /**
     * @brief Retrieve the absolute byte offset within the source view.
     * @return A value in the closed interval `[0, view().size()]`.
     */
    [[nodiscard]] std::size_t offset() const noexcept
    {
        return index_;
    }

    /**
     * @brief Consume consecutive C-locale whitespace from the current offset.
     *
     * Line and column tracking is updated for every consumed byte. The first
     * non-whitespace byte, if any, remains unconsumed.
     */
    void skipWs() noexcept;

    /**
     * @brief Consume one byte when it exactly matches `c`.
     * @param c Character expected at the current position.
     * @return `true` when `c` matched and was consumed; otherwise `false` with
     *         the cursor unchanged.
     */
    bool consume(char c) noexcept;

    /**
     * @brief Conditionally consume one exact byte.
     * @param c Character to compare with `peek()`.
     * @return `true` when `c` was consumed; otherwise `false` with the cursor
     *         unchanged.
     *
     * @note This is equivalent in behavior to `consume(c)` and does not skip
     *       leading whitespace.
     */
    bool consumeIf(char c) noexcept;

    /**
     * @brief Consume the maximal byte sequence accepted by a predicate.
     *
     * @tparam Predicate Callable satisfying `CursorPredicate`.
     * @param pred Predicate invoked on each current byte. The first rejected
     *             byte is left unconsumed.
     * @return A borrowed view of all bytes consumed, possibly empty.
     *
     * @note Leading whitespace is not skipped automatically.
     */
    template <CursorPredicate Predicate> std::string_view consumeWhile(Predicate pred) noexcept
    {
        const std::size_t begin = index_;
        while (!atEnd() && pred(peek()))
            advance();
        return text_.substr(begin, index_ - begin);
    }

    /**
     * @brief Skip whitespace and consume an IL parser identifier.
     *
     * Identifiers begin with an alphabetic byte, underscore, or dot. Remaining
     * bytes may additionally contain digits and dollar signs.
     *
     * @param[out] out On success, receives a borrowed view of the identifier;
     *                 it is unchanged on failure.
     * @return `true` when an identifier was consumed.
     *
     * @note Leading whitespace remains consumed even when no identifier follows.
     */
    bool consumeIdent(std::string_view &out) noexcept;

    /**
     * @brief Skip whitespace and consume an optionally signed decimal integer.
     *
     * @param[out] out On success, receives a borrowed view including any leading
     *                 sign; it is unchanged on failure.
     * @return `true` when at least one decimal digit was consumed.
     *
     * @note If a sign is not followed by a digit, scanning rewinds to the first
     *       non-whitespace byte; already skipped whitespace remains consumed.
     */
    bool consumeNumber(std::string_view &out) noexcept;

    /**
     * @brief Skip whitespace and consume a case-sensitive keyword token.
     *
     * A match is rejected when the following byte could continue an identifier,
     * preventing a keyword prefix from splitting a longer name.
     *
     * @param kw Non-empty keyword to match exactly.
     * @return `true` when the full keyword was consumed.
     *
     * @note Leading whitespace remains consumed when matching fails.
     */
    bool consumeKeyword(std::string_view kw) noexcept;

    /**
     * @brief Consume one byte and update the tracked source position.
     *
     * A newline increments the line coordinate and resets the column to zero;
     * any other byte increments the column. Calling at end of input is a no-op.
     */
    void advance() noexcept;

    /**
     * @brief Reposition the cursor at an absolute byte offset.
     *
     * Forward seeks update the position from the current state. Backward seeks
     * replay the source from the constructor's origin so line and column remain
     * accurate.
     *
     * @param offset Requested byte offset, clamped to `view().size()`.
     * @post `offset()` equals `min(offset, view().size())`.
     */
    void seek(std::size_t offset) noexcept;

    /**
     * @brief Move directly to end of input while updating source coordinates.
     * @post `atEnd()` is true and `remaining()` is empty.
     */
    void consumeRest() noexcept
    {
        seek(text_.size());
    }

  private:
    /**
     * @brief Incorporate one already-consumed byte into source coordinates.
     * @param ch Byte removed from the source; newline advances the line and
     *           resets the column, while every other byte advances the column.
     */
    void applyAdvance(char ch) noexcept;

    std::string_view text_;
    std::size_t index_ = 0;
    SourcePos start_{};
    SourcePos pos_{};
};

} // namespace zanna::parse
