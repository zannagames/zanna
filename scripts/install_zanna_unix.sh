#!/usr/bin/env bash
#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/install_zanna_unix.sh
# Purpose: Canonical POSIX Zanna install step. Installs an already-built tree
#          without configuring, rebuilding, or testing, so an install that
#          failed (e.g. the sudo prompt timed out at the end of
#          build_zanna_unix.sh) can be finished on its own.
#
# Key invariants:
#   - Never builds; fails fast if the build tree was never configured.
#   - Honors the same ZANNA_BUILD_DIR / ZANNA_INSTALL_PREFIX as the build
#     driver and holds the same per-build-directory lock.
#   - A default /usr/local prefix falls back to <build>/install when sudo is
#     needed but stdin is not a terminal.
#
# Ownership/Lifetime:
#   - Holds the build lock until exit; adopts it when exec'd by
#     build_zanna_unix.sh.
#
# Links: scripts/build_zanna_unix.sh, scripts/install_zanna_linux.sh,
#        scripts/install_zanna_mac.sh, scripts/zanna_build_lock.sh
#
#===----------------------------------------------------------------------===#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

case "$(uname -s 2>/dev/null || true)" in
    Darwin) RETRY_SCRIPT="./scripts/install_zanna_mac.sh" ;;
    Linux) RETRY_SCRIPT="./scripts/install_zanna_linux.sh" ;;
    *)
        echo "Error: install_zanna_unix.sh supports macOS and Linux only"
        exit 1
        ;;
esac

BUILD_DIR="${ZANNA_BUILD_DIR:-$ROOT_DIR/build}"
INSTALL_PREFIX="${ZANNA_INSTALL_PREFIX:-/usr/local}"
INSTALL_PREFIX_EXPLICIT=0
if [[ -n "${ZANNA_INSTALL_PREFIX+x}" ]]; then
    INSTALL_PREFIX_EXPLICIT=1
fi

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "error: build directory not found: $BUILD_DIR" >&2
    echo "Run the build script first (./scripts/build_zanna_unix.sh)." >&2
    exit 1
fi
BUILD_DIR="$(cd "$BUILD_DIR" && pwd -P)"
if [[ ! -f "$BUILD_DIR/cmake_install.cmake" ]]; then
    echo "error: $BUILD_DIR is not a configured Zanna build tree (missing cmake_install.cmake)" >&2
    echo "Run the build script first (./scripts/build_zanna_unix.sh)." >&2
    exit 1
fi

BUILD_LOCK_PATH="$BUILD_DIR/.zanna-build.lock"
# shellcheck source=SCRIPTDIR/zanna_build_lock.sh
source "$SCRIPT_DIR/zanna_build_lock.sh"
acquire_build_lock

install_failed() {
    echo "error: install to $INSTALL_PREFIX failed; the build itself is intact." >&2
    echo "Re-run $RETRY_SCRIPT to finish the install without rebuilding." >&2
    exit 1
}

echo "[install_zanna] Installing $BUILD_DIR to $INSTALL_PREFIX..."
if [[ "$(id -u)" -eq 0 ]]; then
    cmake --install "$BUILD_DIR" --prefix "$INSTALL_PREFIX" || install_failed
elif { [[ -d "$INSTALL_PREFIX" ]] || mkdir -p "$INSTALL_PREFIX" 2>/dev/null; } && [[ -w "$INSTALL_PREFIX" ]]; then
    cmake --install "$BUILD_DIR" --prefix "$INSTALL_PREFIX" || install_failed
elif [[ ! -t 0 ]]; then
    if [[ "$INSTALL_PREFIX_EXPLICIT" != "1" && "$INSTALL_PREFIX" == "/usr/local" ]]; then
        INSTALL_PREFIX="$BUILD_DIR/install"
        echo "[install_zanna] Non-interactive install: /usr/local requires sudo; using $INSTALL_PREFIX"
        cmake --install "$BUILD_DIR" --prefix "$INSTALL_PREFIX" || install_failed
    else
        echo "error: install to $INSTALL_PREFIX requires sudo, but stdin is not a terminal" >&2
        echo "Run $RETRY_SCRIPT from a terminal, or set ZANNA_INSTALL_PREFIX to a writable prefix." >&2
        exit 1
    fi
else
    echo "[install_zanna] sudo credentials are required for install"
    sudo -v || install_failed
    sudo cmake --install "$BUILD_DIR" --prefix "$INSTALL_PREFIX" || install_failed
fi
echo "[install_zanna] Install complete"
