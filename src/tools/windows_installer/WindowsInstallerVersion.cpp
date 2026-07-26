//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file WindowsInstallerVersion.cpp
/// @brief Implements installer-version comparison with SemVer-compatible prerelease rules.
///
/// Build metadata never affects precedence. Numeric prerelease identifiers compare numerically and
/// sort before text identifiers, while dotted numeric cores retain compatibility with Zanna's
/// four-part builds. All helpers are pure and retain no state.
//
//===----------------------------------------------------------------------===//

#include "WindowsInstallerHost.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace zanna::installer {
namespace {

/// @brief Validated precedence-bearing portions of an installer version.
struct ParsedVersion {
    /// @brief Dotted numeric core, padded to at least three components.
    std::vector<int> core;
    /// @brief Dot-delimited prerelease text without the leading hyphen.
    std::string prerelease;
};

/// @brief Validate dot-delimited prerelease or build-metadata identifiers.
/// @param value Nonempty identifier sequence to validate.
/// @param field Field name included in diagnostics.
/// @param numericLeadingZero Whether multi-digit numeric identifiers must reject a leading zero.
/// @throws std::runtime_error If punctuation, characters, emptiness, or numeric padding is invalid.
void validateIdentifiers(std::string_view value, std::string_view field, bool numericLeadingZero) {
    /// @brief Identify a byte forbidden in semantic-version identifiers.
    /// @param ch Byte to inspect.
    /// @return `true` unless `ch` is alphanumeric, a hyphen, or a period.
    if (value.empty() || value.front() == '.' || value.back() == '.' ||
        value.find("..") != std::string_view::npos ||
        std::any_of(value.begin(), value.end(), [](unsigned char ch) {
            return !(std::isalnum(ch) || ch == '-' || ch == '.');
        })) {
        throw std::runtime_error("package version has invalid " + std::string(field));
    }
    if (!numericLeadingZero)
        return;
    std::size_t start = 0;
    for (;;) {
        const std::size_t dot = value.find('.', start);
        const std::string_view identifier =
            value.substr(start, dot == std::string_view::npos ? value.size() - start : dot - start);
        /// @brief Test whether one prerelease-identifier byte is an ASCII digit.
        /// @param ch Byte to inspect.
        /// @return `true` for `0` through `9`.
        const bool numeric = std::all_of(
            identifier.begin(), identifier.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
        if (numeric && identifier.size() > 1U && identifier.front() == '0')
            throw std::runtime_error("package version has a zero-padded numeric prerelease");
        if (dot == std::string_view::npos)
            break;
        start = dot + 1U;
    }
}

/// @brief Parse and validate a supported semantic installer version.
/// @param version Version text with an optional prerelease and build suffix.
/// @return Numeric core and precedence-bearing prerelease identifiers.
/// @throws std::runtime_error If core, prerelease, build metadata, or integer range is invalid.
ParsedVersion parseInstallerVersion(std::string_view version) {
    ParsedVersion result;
    const std::size_t plus = version.find('+');
    if (plus != std::string_view::npos) {
        validateIdentifiers(version.substr(plus + 1U), "build metadata", false);
        version = version.substr(0, plus);
    }
    const std::size_t dash = version.find('-');
    if (dash != std::string_view::npos) {
        result.prerelease = std::string(version.substr(dash + 1U));
        validateIdentifiers(result.prerelease, "prerelease identifier", true);
        version = version.substr(0, dash);
    }
    std::size_t start = 0;
    while (start <= version.size()) {
        const std::size_t dot = version.find('.', start);
        const std::string_view part = version.substr(
            start, dot == std::string_view::npos ? version.size() - start : dot - start);
        if (part.empty() || part.size() > 9U || (part.size() > 1U && part.front() == '0'))
            throw std::runtime_error("package version is not a supported semantic version");
        int parsedValue = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), parsedValue);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || parsedValue < 0)
            throw std::runtime_error("package version is not a supported semantic version");
        result.core.push_back(parsedValue);
        if (dot == std::string_view::npos)
            break;
        start = dot + 1U;
    }
    while (result.core.size() < 3U)
        result.core.push_back(0);
    return result;
}

/// @brief Test whether every byte in an identifier is an ASCII decimal digit.
/// @param value Nonempty validated prerelease identifier.
/// @return @c true when the identifier is numeric.
bool numericIdentifier(std::string_view value) {
    /// @brief Test whether one identifier byte is an ASCII digit.
    /// @param ch Byte to inspect.
    /// @return `true` for `0` through `9`.
    return std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}

/// @brief Compare canonical nonnegative decimal identifiers without integer conversion.
/// @param left First numeric identifier without leading zero padding.
/// @param right Second numeric identifier without leading zero padding.
/// @return Negative, zero, or positive according to numeric ordering.
int compareNumeric(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return left.size() < right.size() ? -1 : 1;
    if (left == right)
        return 0;
    return left < right ? -1 : 1;
}

/// @brief Compare dot-delimited prerelease sequences using semantic-version precedence.
/// @param left First validated prerelease sequence.
/// @param right Second validated prerelease sequence.
/// @return Negative, zero, or positive according to prerelease ordering.
int comparePrerelease(std::string_view left, std::string_view right) {
    std::size_t leftStart = 0;
    std::size_t rightStart = 0;
    for (;;) {
        const bool leftDone = leftStart >= left.size();
        const bool rightDone = rightStart >= right.size();
        if (leftDone || rightDone) {
            if (leftDone == rightDone)
                return 0;
            return leftDone ? -1 : 1;
        }
        const std::size_t leftDot = left.find('.', leftStart);
        const std::size_t rightDot = right.find('.', rightStart);
        const std::string_view leftId = left.substr(
            leftStart,
            leftDot == std::string_view::npos ? left.size() - leftStart : leftDot - leftStart);
        const std::string_view rightId = right.substr(
            rightStart,
            rightDot == std::string_view::npos ? right.size() - rightStart : rightDot - rightStart);
        const bool leftNumeric = numericIdentifier(leftId);
        const bool rightNumeric = numericIdentifier(rightId);
        int comparison = 0;
        if (leftNumeric && rightNumeric)
            comparison = compareNumeric(leftId, rightId);
        else if (leftNumeric != rightNumeric)
            comparison = leftNumeric ? -1 : 1;
        else if (leftId != rightId)
            comparison = leftId < rightId ? -1 : 1;
        if (comparison != 0)
            return comparison;
        leftStart = leftDot == std::string_view::npos ? left.size() : leftDot + 1U;
        rightStart = rightDot == std::string_view::npos ? right.size() : rightDot + 1U;
    }
}

} // namespace

/// @brief Compare two installer versions using numeric core and SemVer prerelease precedence.
/// @param left First version string.
/// @param right Second version string.
/// @return Negative when @p left is older, zero when precedence is equal, or positive when newer.
/// @throws std::runtime_error If either version is outside the supported syntax.
int compareInstallerVersions(std::string_view left, std::string_view right) {
    ParsedVersion parsedLeft = parseInstallerVersion(left);
    ParsedVersion parsedRight = parseInstallerVersion(right);
    const std::size_t count = (std::max)(parsedLeft.core.size(), parsedRight.core.size());
    parsedLeft.core.resize(count);
    parsedRight.core.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (parsedLeft.core[index] != parsedRight.core[index])
            return parsedLeft.core[index] < parsedRight.core[index] ? -1 : 1;
    }
    if (parsedLeft.prerelease.empty() != parsedRight.prerelease.empty())
        return parsedLeft.prerelease.empty() ? 1 : -1;
    if (parsedLeft.prerelease == parsedRight.prerelease)
        return 0;
    return comparePrerelease(parsedLeft.prerelease, parsedRight.prerelease);
}

} // namespace zanna::installer
