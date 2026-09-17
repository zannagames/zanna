#!/usr/bin/env bash
#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/install_zanna_linux.sh
# Purpose: Linux wrapper for the shared Unix install step. Use it to finish an
#          install that failed at the end of build_zanna_linux.sh (for example
#          when the sudo prompt timed out) without rebuilding.
#
# Key invariants:
#   - Invocation is independent of the caller's working directory.
#   - Never builds or tests; only installs the existing build tree.
#
# Ownership/Lifetime:
#   - Replaces itself with install_zanna_unix.sh; retains no process state.
#
# Links: scripts/install_zanna_unix.sh, scripts/build_zanna_linux.sh
#
#===----------------------------------------------------------------------===#

set -euo pipefail

if [[ "$(uname -s 2>/dev/null)" != "Linux" ]]; then
    echo "Error: install_zanna_linux.sh must be run on Linux"
    exit 1
fi

SCRIPT_PATH="$(readlink -f -- "${BASH_SOURCE[0]}")"
if [[ -z "$SCRIPT_PATH" ]]; then
    echo "Error: could not resolve install_zanna_linux.sh"
    exit 1
fi
SCRIPT_DIR="$(cd "$(dirname "$SCRIPT_PATH")" && pwd)"
exec "$SCRIPT_DIR/install_zanna_unix.sh" "$@"
