//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTInputLineFailTests.cpp
// Purpose: Verify rt_input_line returns NULL when buffer expansion fails.
// Key invariants: Function aborts reading on realloc failure and reports trap.
// Ownership/Lifetime: Uses runtime library; stubs realloc and trap for simulation.
// Links: docs/runtime-vm.md#runtime-abi
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "tests/common/PlatformSkip.h"
#include "tests/common/PosixCompat.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
// This test requires overriding realloc which doesn't work with Windows DLL CRT
int main() {
    ZANNA_PLATFORM_SKIP("Cannot override CRT functions on Windows DLL");
}
#else

static const char *g_msg = nullptr;

// Stub realloc to simulate allocation failure.
extern "C" void *realloc(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
    return NULL;
}

// Stub vm_trap to capture message without aborting.
extern "C" void vm_trap(const char *msg) {
    g_msg = msg;
}

int main() {
    // Create a temporary file with test input instead of using a pipe
    // This avoids stdin redirection issues across platforms
    char tmpname[] = "/tmp/zanna_input_test_XXXXXX";
    int tmpfd = mkstemp(tmpname);
    assert(tmpfd >= 0);

    std::string input(1500, 'x');
    const ssize_t written = write(tmpfd, input.data(), input.size());
    assert(written == static_cast<ssize_t>(input.size()));
    close(tmpfd);

    // Redirect stdin to the temp file
    FILE *new_stdin = freopen(tmpname, "r", stdin);
    assert(new_stdin != nullptr);

    // Clean up temp file (it remains open via stdin)
    unlink(tmpname);

    rt_string s = rt_input_line();
    assert(!s);
    assert(g_msg && std::strcmp(g_msg, "out of memory") == 0);
    return 0;
}
#endif // !_WIN32
