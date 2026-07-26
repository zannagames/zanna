//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/io/rt_io_class_ids.h
// Purpose: Stable negative runtime class identifiers for opaque Zanna.IO heap handles. These tags
//          let shared object-validation and destruction paths distinguish unrelated native I/O
//          payloads without exposing their private layouts.
//
// Key invariants:
//   - Every opaque I/O handle type has one unique identifier in this reserved negative range.
//   - Identifiers are compile-time int64_t constants and must remain stable anywhere a handle can
//     cross runtime module boundaries.
//   - A class identifier validates only the outer runtime object kind; individual modules still
//     enforce handle state, ownership, and lifecycle rules.
//
// Ownership/Lifetime:
//   - This header declares values only and owns no storage.
//   - Objects carrying these tags are managed by the implementing runtime module and the common
//     object reference-counting machinery.
//
// Links: src/runtime/io/rt_binfile.h, src/runtime/io/rt_memstream.h,
//        src/runtime/io/rt_stream.h, src/runtime/io/rt_archive.h,
//        src/runtime/io/rt_savedata.h, src/runtime/io/rt_watcher.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Defines stable runtime class identifiers for opaque I/O payloads.
 * @details These negative tags distinguish managed binary files, streams,
 * wrappers, archives, save data, watchers, and binary buffers across module
 * boundaries without exposing their private object layouts.
 */
#pragma once

#include <stdint.h>

/// @brief Class identifier for opaque binary-file handles.
#define RT_BINFILE_CLASS_ID INT64_C(-0x760001)

/// @brief Class identifier for opaque in-memory stream handles.
#define RT_MEMSTREAM_CLASS_ID INT64_C(-0x760002)

/// @brief Class identifier for generic opaque stream handles.
#define RT_STREAM_CLASS_ID INT64_C(-0x760003)

/// @brief Class identifier for line-reader wrapper handles.
#define RT_LINEREADER_CLASS_ID INT64_C(-0x760004)

/// @brief Class identifier for line-writer wrapper handles.
#define RT_LINEWRITER_CLASS_ID INT64_C(-0x760005)

/// @brief Class identifier for archive container handles.
#define RT_ARCHIVE_CLASS_ID INT64_C(-0x760006)

/// @brief Class identifier for structured save-data handles.
#define RT_SAVEDATA_CLASS_ID INT64_C(-0x760007)

/// @brief Class identifier for filesystem watcher handles.
#define RT_WATCHER_CLASS_ID INT64_C(-0x760008)

/// @brief Class identifier for binary-buffer handles.
#define RT_BINBUF_CLASS_ID INT64_C(-0x760009)
