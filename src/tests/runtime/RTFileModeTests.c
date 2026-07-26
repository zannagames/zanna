//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTFileModeTests.c
// Purpose: Verify runtime file mode parsing maps binary modifiers to platform flags.
// Key invariants: Binary modifier must set the platform-specific binary bit on Windows builds.
// Ownership/Lifetime: Uses static mode strings; relies on runtime library for parsing.
// Links: docs/internals/codemap.md
#include "rt_file.h"
#include "rt_file_path.h"

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>

/// @brief Validate flag bits produced by parsing representative mode strings.
int main(void) {
#ifdef _WIN32
    int flags = 0;
    assert(rt_file_mode_to_flags("rb+", RT_F_BINARY, &flags));

    int binary_mask = 0;
#ifdef O_BINARY
    binary_mask |= O_BINARY;
#endif
#ifdef _O_BINARY
    binary_mask |= _O_BINARY;
#endif
    assert(binary_mask != 0);
    assert((flags & binary_mask) == binary_mask);
#else
    int flags = 0;
    assert(rt_file_mode_to_flags("rb+", RT_F_BINARY, &flags));
#endif

    assert((flags & O_CREAT) == O_CREAT);
    assert((flags & O_RDWR) == O_RDWR);

    int random_flags = 0;
    assert(rt_file_mode_to_flags("rb+", RT_F_RANDOM, &random_flags));
    assert((random_flags & O_CREAT) == O_CREAT);
    assert((random_flags & O_RDWR) == O_RDWR);

#ifdef _WIN32
    assert((random_flags & binary_mask) == binary_mask);
#endif

    int text_flags = 0;
    assert(rt_file_mode_to_flags("rt", RT_F_UNSPECIFIED, &text_flags));
#ifdef _WIN32
    assert((text_flags & binary_mask) == 0);
#endif
    assert((text_flags & O_CREAT) == 0);

    int unchanged_flags = 0x5A5A;
    assert(!rt_file_mode_to_flags("rx", RT_F_UNSPECIFIED, &unchanged_flags));
    assert(unchanged_flags == 0x5A5A);
    assert(!rt_file_mode_to_flags(NULL, RT_F_UNSPECIFIED, &unchanged_flags));
    assert(unchanged_flags == 0x5A5A);

    return 0;
}
