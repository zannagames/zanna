#!/usr/bin/env bash
#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/install_zanna_mac.sh
# Purpose: macOS wrapper for the shared Unix install step. Use it to finish an
#          install that failed at the end of build_zanna_mac.sh (for example
#          when the sudo prompt timed out) without rebuilding.
#
# Key invariants:
#   - Invocation is independent of the caller's working directory.
#   - Never builds or tests; only installs the existing build tree.
#
# Ownership/Lifetime:
#   - Replaces itself with install_zanna_unix.sh; retains no process state.
#
# Links: scripts/install_zanna_unix.sh, scripts/build_zanna_mac.sh
#
#===----------------------------------------------------------------------===#

set -euo pipefail

if [[ "$(uname -s 2>/dev/null)" != "Darwin" ]]; then
    echo "Error: install_zanna_mac.sh must be run on macOS"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/install_zanna_unix.sh" "$@"
