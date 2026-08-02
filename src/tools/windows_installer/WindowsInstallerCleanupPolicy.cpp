//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/windows_installer/WindowsInstallerCleanupPolicy.cpp
// Purpose: Implement fail-closed command-line and exact-path validation for the
//          detached Windows installer cleanup helper.
// Key invariants:
//   - Accepted paths have one stable, well-formed Win32 interpretation below a root.
//   - Equality follows Windows ordinal case rules and normalizes safe namespace aliases.
// Ownership/Lifetime:
//   - All string views are borrowed and never retained.
//   - Validation performs no allocation and never accesses the filesystem.
// Links: src/tools/windows_installer/WindowsInstallerCleanupPolicy.hpp
//
//===----------------------------------------------------------------------===//

#include "WindowsInstallerCleanupPolicy.hpp"

#include <windows.h>

#include <cstddef>
#include <limits>

namespace zanna::installer::cleanup {
namespace {

/// @brief Test whether a path character is a Windows directory separator.
/// @param ch Candidate UTF-16 code unit.
/// @return @c true for slash or backslash.
constexpr bool isSeparator(wchar_t ch) noexcept {
    return ch == L'\\' || ch == L'/';
}

/// @brief Test whether a code unit is an ASCII drive-letter character.
/// @param ch Candidate UTF-16 code unit.
/// @return @c true for ASCII A-Z or a-z.
constexpr bool isAsciiAlpha(wchar_t ch) noexcept {
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

/// @brief Fold an ASCII lowercase code unit to uppercase.
/// @param ch Candidate UTF-16 code unit.
/// @return Uppercase ASCII equivalent, or @p ch unchanged.
constexpr wchar_t asciiUpper(wchar_t ch) noexcept {
    return ch >= L'a' && ch <= L'z' ? static_cast<wchar_t>(ch - (L'a' - L'A')) : ch;
}

/// @brief Compare a literal path prefix using ASCII-only case folding.
/// @param text Candidate complete path.
/// @param prefix Prefix to match.
/// @return @c true when every prefix code unit matches under ASCII folding.
bool startsWithAsciiInsensitive(std::wstring_view text, std::wstring_view prefix) noexcept {
    if (text.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (asciiUpper(text[i]) != asciiUpper(prefix[i]))
            return false;
    }
    return true;
}

/// @brief Validate UTF-16 surrogate pairing before a path reaches Win32.
/// @param text Candidate UTF-16 string.
/// @return @c true when every high surrogate has one low surrogate and no low
///         surrogate appears by itself.
bool isWellFormedUtf16(std::wstring_view text) noexcept {
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch >= 0xd800 && ch <= 0xdbff) {
            if (++i >= text.size() || text[i] < 0xdc00 || text[i] > 0xdfff)
                return false;
        } else if (ch >= 0xdc00 && ch <= 0xdfff) {
            return false;
        }
    }
    return true;
}

/// @brief Test whether a component's base spelling names a Win32 device.
/// @param component Single path component, possibly with an extension.
/// @return @c true for reserved names such as CON, NUL, COM1, or LPT1.
bool isReservedDeviceBase(std::wstring_view component) noexcept {
    const size_t dot = component.find(L'.');
    const std::wstring_view base = component.substr(0, dot);
    /// @brief Compare the component base with one uppercase reserved device name.
    /// @param expected Uppercase device name to compare.
    /// @return `true` when `base` equals `expected` under ASCII case folding.
    auto equals = [base](std::wstring_view expected) noexcept {
        if (base.size() != expected.size())
            return false;
        for (size_t i = 0; i < base.size(); ++i) {
            if (asciiUpper(base[i]) != expected[i])
                return false;
        }
        return true;
    };

    if (equals(L"CON") || equals(L"PRN") || equals(L"AUX") || equals(L"NUL") || equals(L"CLOCK$") ||
        equals(L"CONIN$") || equals(L"CONOUT$")) {
        return true;
    }
    if (base.size() == 4) {
        const wchar_t first = asciiUpper(base[0]);
        const wchar_t second = asciiUpper(base[1]);
        const wchar_t third = asciiUpper(base[2]);
        const wchar_t digit = base[3];
        const bool numberedDevice = (first == L'C' && second == L'O' && third == L'M') ||
                                    (first == L'L' && second == L'P' && third == L'T');
        if (numberedDevice && ((digit >= L'1' && digit <= L'9') || digit == L'\u00b9' ||
                               digit == L'\u00b2' || digit == L'\u00b3')) {
            return true;
        }
    }
    return false;
}

/// @brief Validate one unambiguous cleanup-path component.
/// @param component Component text without separators.
/// @return @c true when nonempty, nontraversing, free of forbidden characters,
///         stable under Win32 normalization, and not a reserved device.
bool isSafeComponent(std::wstring_view component) noexcept {
    if (component.empty() || component == L"." || component == L"..")
        return false;
    if (component.back() == L'.' || component.back() == L' ')
        return false;
    for (const wchar_t ch : component) {
        if (ch < 0x20 || ch == 0x7f || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'|' || ch == L'?' || ch == L'*' || isSeparator(ch)) {
            return false;
        }
    }
    return !isReservedDeviceBase(component);
}

/// @brief Consume and validate a UNC server/share root.
/// @param path Complete UNC path.
/// @param start Offset of the server component.
/// @param cursor Receives the offset immediately after the share separator.
/// @return @c true when both server and share are safe nonempty components.
bool consumeUncRoot(std::wstring_view path, size_t start, size_t &cursor) noexcept {
    cursor = start;
    for (unsigned rootPart = 0; rootPart < 2; ++rootPart) {
        const size_t componentStart = cursor;
        while (cursor < path.size() && !isSeparator(path[cursor]))
            ++cursor;
        if (cursor == componentStart ||
            !isSafeComponent(path.substr(componentStart, cursor - componentStart))) {
            return false;
        }
        if (cursor == path.size())
            return false;
        ++cursor;
    }
    return true;
}

} // namespace

/// @brief Validate an absolute drive or UNC cleanup target.
/// @param path Candidate normal or extended-length Windows path.
/// @return @c true only when the path is bounded, NUL-free, lies beneath a
///         recognized root, and contains safe nonempty child components.
bool isSafeAbsolutePath(std::wstring_view path) noexcept {
    if (path.size() < 4 || path.size() >= 32760 || path.find(L'\0') != std::wstring_view::npos ||
        !isWellFormedUtf16(path)) {
        return false;
    }

    size_t cursor = 0;
    const bool isExtended = path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' &&
                            path[2] == L'?' && path[3] == L'\\';
    if (isExtended) {
        // Extended paths are passed to Win32 literally, so accepting slash aliases
        // here would validate a spelling different from the one Win32 interprets.
        if (path.find(L'/') != std::wstring_view::npos)
            return false;
        if (path.size() >= 7 && isAsciiAlpha(path[4]) && path[5] == L':' && path[6] == L'\\') {
            cursor = 7;
        } else if (path.size() >= 8 && startsWithAsciiInsensitive(path, L"\\\\?\\UNC\\")) {
            if (!consumeUncRoot(path, 8, cursor))
                return false;
        } else {
            return false;
        }
    } else if (isAsciiAlpha(path[0]) && path[1] == L':' && isSeparator(path[2])) {
        cursor = 3;
    } else if (isSeparator(path[0]) && isSeparator(path[1]) && path[2] != L'.' && path[2] != L'?') {
        if (!consumeUncRoot(path, 2, cursor))
            return false;
    } else {
        return false;
    }

    if (cursor >= path.size())
        return false;
    while (cursor < path.size()) {
        const size_t componentStart = cursor;
        while (cursor < path.size() && !isSeparator(path[cursor]))
            ++cursor;
        if (!isSafeComponent(path.substr(componentStart, cursor - componentStart)))
            return false;
        if (cursor < path.size()) {
            ++cursor;
            if (cursor == path.size())
                return false;
        }
    }
    return true;
}

/// @brief Canonical view of a validated drive or UNC path.
struct ComparablePath {
    std::wstring_view body;
    bool unc{false};
};

/// @brief Remove an accepted extended-length prefix without allocating.
/// @param path Validated absolute path.
/// @param comparable Receives a comparable root kind and path body.
/// @return @c true when the path uses a recognized drive or UNC spelling.
bool makeComparablePath(std::wstring_view path, ComparablePath &comparable) noexcept {
    if (path.size() >= 8 && startsWithAsciiInsensitive(path, L"\\\\?\\UNC\\")) {
        comparable = {path.substr(8), true};
        return true;
    }
    if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' &&
        path[3] == L'\\') {
        comparable = {path.substr(4), false};
        return true;
    }
    if (path.size() >= 2 && isSeparator(path[0]) && isSeparator(path[1])) {
        comparable = {path.substr(2), true};
        return true;
    }
    if (path.size() >= 3 && isAsciiAlpha(path[0]) && path[1] == L':' && isSeparator(path[2])) {
        comparable = {path, false};
        return true;
    }
    return false;
}

/// @brief Compare one path component using Win32's locale-independent case rules.
/// @param left First component.
/// @param right Second component.
/// @return @c true when CompareStringOrdinal reports equality ignoring case.
bool componentsEqual(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return CompareStringOrdinal(left.data(),
                                static_cast<int>(left.size()),
                                right.data(),
                                static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

/// @brief Compare Windows path spellings without filesystem access.
/// @param left First validated path.
/// @param right Second validated path.
/// @return @c true when namespace aliases, separators, and Unicode case compare
///         equal under Windows ordinal rules.
bool pathsEqual(std::wstring_view left, std::wstring_view right) noexcept {
    ComparablePath leftPath;
    ComparablePath rightPath;
    if (!makeComparablePath(left, leftPath) || !makeComparablePath(right, rightPath) ||
        leftPath.unc != rightPath.unc) {
        return false;
    }

    size_t leftCursor = 0;
    size_t rightCursor = 0;
    for (;;) {
        size_t leftEnd = leftCursor;
        size_t rightEnd = rightCursor;
        while (leftEnd < leftPath.body.size() && !isSeparator(leftPath.body[leftEnd]))
            ++leftEnd;
        while (rightEnd < rightPath.body.size() && !isSeparator(rightPath.body[rightEnd]))
            ++rightEnd;
        if (!componentsEqual(leftPath.body.substr(leftCursor, leftEnd - leftCursor),
                             rightPath.body.substr(rightCursor, rightEnd - rightCursor))) {
            return false;
        }
        const bool leftDone = leftEnd == leftPath.body.size();
        const bool rightDone = rightEnd == rightPath.body.size();
        if (leftDone || rightDone)
            return leftDone && rightDone;
        leftCursor = leftEnd + 1;
        rightCursor = rightEnd + 1;
    }
}

/// @brief Parse one strict nonzero decimal process identifier.
/// @param text Candidate decimal text.
/// @param processId Receives the parsed value on success and zero on failure.
/// @return @c true when every character is a digit and the value fits uint32_t.
bool parseProcessId(std::wstring_view text, std::uint32_t &processId) noexcept {
    processId = 0;
    if (text.empty())
        return false;
    std::uint32_t value = 0;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9')
            return false;
        const std::uint32_t digit = static_cast<std::uint32_t>(ch - L'0');
        if (value > ((std::numeric_limits<std::uint32_t>::max)() - digit) / 10U)
            return false;
        value = value * 10U + digit;
    }
    if (value == 0)
        return false;
    processId = value;
    return true;
}

} // namespace zanna::installer::cleanup
