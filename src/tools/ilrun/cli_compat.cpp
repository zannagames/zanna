//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Compatibility shims for zanna CLI functions used by cmd_run_il.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Provides the standalone ilrun usage callback expected by shared CLI code.

#include <iostream>

/// @brief Usage callback used by cmd_run_il.cpp when linked into ilrun.
void usage() {
    std::cerr << "Usage: ilrun [options] <file.il>\n"
              << "\n"
              << "Options:\n"
              << "  --trace[=il|src]        Enable execution tracing\n"
              << "  --stdin-from FILE       Redirect stdin from file\n"
              << "  --max-steps N           Limit execution steps\n"
              << "  --bounds-checks         Require precompiled bounds checks (source compile only)\n"
              << "  --break LABEL|FILE:LINE Set breakpoint\n"
              << "  --break-src FILE:LINE   Set source breakpoint\n"
              << "  --watch NAME            Watch variable\n"
              << "  --count                 Show instruction counts\n"
              << "  --time                  Show execution time\n"
              << "  --dump-trap             Show detailed trap diagnostics\n"
              << "  -h, --help              Show help\n";
}
