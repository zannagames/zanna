//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/il/IO.hpp
// Purpose: Stable façade for IL text parsing, serialization, and string utilities.
// Key invariants: Re-exports supported IO interfaces; parser internals stay internal.
// Ownership/Lifetime: Parser/Serializer mirror underlying implementations.
// Links: docs/il/il-guide.md#reference, src/il/io/
//
//===----------------------------------------------------------------------===//

#pragma once

#include "il/io/Parser.hpp"
#include "il/io/Serializer.hpp"
#include "il/io/StringEscape.hpp"

/**
 * @file include/zanna/il/IO.hpp
 * @brief Aggregates the supported public interfaces for textual IL input and
 *        output.
 *
 * @details
 * Including this façade exposes the high-level parser, serializer, and string
 * escape helpers without requiring clients to know their source-tree paths.
 * It deliberately adds no declarations or behavior of its own; ownership,
 * error reporting, and lifetime rules are those documented by the three
 * forwarded interfaces.
 *
 * Internal parser state and token-specific helpers remain outside this public
 * include surface.
 */
