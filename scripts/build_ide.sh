#!/usr/bin/env bash
#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/build_ide.sh
# Purpose: Build and stage Zanna Studio native outputs on Unix hosts.
# Key invariants:
#   - Uses an existing Zanna compiler from the configured build tree.
#   - Writes build metadata beside both primary and compatibility outputs.
#   - macOS keeps the `zannastudio` command while executing a sibling native
#     payload named `Zanna Studio` for the authored Cocoa application identity.
# Ownership/Lifetime:
#   - Temporary diagnostics are removed on exit; built outputs remain caller-owned.
# Cross-platform touchpoints:
#   - Handles Unix and Windows-form paths when an .exe compiler is selected.
# Links: build_ide_win.ps1, src/zannastudio/README.md,
#        docs/adr/0149-macos-zanna-studio-application-identity.md
#
#===----------------------------------------------------------------------===#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${ZANNA_BUILD_DIR:-$ROOT_DIR/build}"
IDE_DIR="$ROOT_DIR/src/zannastudio"
OUT_DIR="${ZANNA_IDE_OUT_DIR:-$IDE_DIR/bin}"
OUTPUT_FILE="${ZANNA_IDE_OUTPUT:-$OUT_DIR/zannastudio}"
COMPAT_OUTPUT_FILE="${ZANNA_IDE_COMPAT_OUTPUT:-$BUILD_DIR/zannastudio/zannastudio}"
SKIP_COMPAT_COPY="${ZANNA_IDE_SKIP_COMPAT_COPY:-0}"
ZANNA_BUILD_TYPE="${ZANNA_BUILD_TYPE:-Debug}"
ZANNA=""
ZANNA_IS_WINDOWS=0
HOST_SYSTEM="$(uname -s 2>/dev/null || printf 'unknown')"
NATIVE_OUTPUT_FILE=""
COMPAT_NATIVE_OUTPUT_FILE=""

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

usage() {
    echo "Usage: $0 [--clean] [--output PATH]"
    echo "  --clean        Remove the existing Zanna Studio binary before building"
    echo "  --output PATH  Write the binary to PATH (default: src/zannastudio/bin/zannastudio)"
    echo "  Compatibility copy: build/zannastudio/zannastudio unless ZANNA_IDE_SKIP_COMPAT_COPY=1"
    exit 1
}

CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            CLEAN=1
            shift
            ;;
        --output)
            if [[ $# -lt 2 ]]; then
                echo "Error: --output requires a path"
                usage
            fi
            OUTPUT_FILE="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown argument: $1"
            usage
            ;;
    esac
done

if [[ ! -d "$IDE_DIR" ]]; then
    echo -e "${RED}Error: Zanna Studio source not found at $IDE_DIR${NC}"
    exit 1
fi

resolve_zanna_tool() {
    local candidate
    local candidates=(
        "$BUILD_DIR/src/tools/zanna/zanna"
        "$BUILD_DIR/src/tools/zanna/zanna.exe"
        "$BUILD_DIR/src/tools/zanna/$ZANNA_BUILD_TYPE/zanna.exe"
        "$BUILD_DIR/src/tools/zanna/Debug/zanna.exe"
        "$BUILD_DIR/src/tools/zanna/Release/zanna.exe"
    )

    for candidate in "${candidates[@]}"; do
        if [[ -x "$candidate" ]]; then
            ZANNA="$candidate"
            if [[ "$candidate" == *.exe ]]; then
                ZANNA_IS_WINDOWS=1
            fi
            return 0
        fi
    done
    return 1
}

windows_path() {
    local path="$1"
    if command -v wslpath >/dev/null 2>&1; then
        wslpath -w "$path"
        return 0
    fi
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$path"
        return 0
    fi
    printf '%s\n' "$path"
}

path_for_zanna() {
    if [[ $ZANNA_IS_WINDOWS -eq 1 ]]; then
        windows_path "$1"
    else
        printf '%s\n' "$1"
    fi
}

if ! resolve_zanna_tool; then
    echo -e "${RED}Error: zanna tool not found under $BUILD_DIR/src/tools/zanna${NC}"
    echo "Run './scripts/build_zanna_mac.sh', './scripts/build_zanna_linux.sh', or 'scripts/build_zanna_win.ps1' first"
    exit 1
fi

if [[ $ZANNA_IS_WINDOWS -eq 1 ]]; then
    if [[ -z "${ZANNA_IDE_OUTPUT:-}" && "$OUTPUT_FILE" == "$OUT_DIR/zannastudio" ]]; then
        OUTPUT_FILE="$OUT_DIR/zannastudio.exe"
    fi
    if [[ -z "${ZANNA_IDE_COMPAT_OUTPUT:-}" && "$COMPAT_OUTPUT_FILE" == "$BUILD_DIR/zannastudio/zannastudio" ]]; then
        COMPAT_OUTPUT_FILE="$BUILD_DIR/zannastudio/zannastudio.exe"
    fi
fi

NATIVE_OUTPUT_FILE="$OUTPUT_FILE"
COMPAT_NATIVE_OUTPUT_FILE="$COMPAT_OUTPUT_FILE"
if [[ "$HOST_SYSTEM" == "Darwin" ]]; then
    NATIVE_OUTPUT_FILE="$(dirname "$OUTPUT_FILE")/Zanna Studio"
    COMPAT_NATIVE_OUTPUT_FILE="$(dirname "$COMPAT_OUTPUT_FILE")/Zanna Studio"
    if [[ "$NATIVE_OUTPUT_FILE" == "$OUTPUT_FILE" ||
          "$COMPAT_NATIVE_OUTPUT_FILE" == "$COMPAT_OUTPUT_FILE" ]]; then
        echo -e "${RED}Error: the macOS launcher path must not be named 'Zanna Studio'${NC}"
        exit 1
    fi
fi

if [[ ! -x "$ZANNA" ]]; then
    echo -e "${RED}Error: zanna tool not found at $ZANNA${NC}"
    echo "Run './scripts/build_zanna_mac.sh' or './scripts/build_zanna_linux.sh' first"
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT_FILE")"

if [[ $CLEAN -eq 1 ]]; then
    rm -f "$OUTPUT_FILE"
    if [[ "$NATIVE_OUTPUT_FILE" != "$OUTPUT_FILE" ]]; then
        rm -f "$NATIVE_OUTPUT_FILE"
    fi
    rm -f "$(dirname "$OUTPUT_FILE")/zannastudio.buildinfo"
    if [[ "$OUTPUT_FILE" != "$COMPAT_OUTPUT_FILE" ]]; then
        rm -f "$COMPAT_OUTPUT_FILE"
        if [[ "$COMPAT_NATIVE_OUTPUT_FILE" != "$COMPAT_OUTPUT_FILE" ]]; then
            rm -f "$COMPAT_NATIVE_OUTPUT_FILE"
        fi
        rm -f "$(dirname "$COMPAT_OUTPUT_FILE")/zannastudio.buildinfo"
    fi
fi

TMP_BASE="/tmp/zannastudio_build_$$"
FRONTEND_ERR="${TMP_BASE}.front.err"

cleanup() {
    rm -f "$FRONTEND_ERR"
}
trap cleanup EXIT

build_macos() {
    local target_arch
    case "$(uname -m)" in
        arm64|aarch64)
            target_arch="arm64"
            ;;
        *)
            echo -e "${RED}Error: macOS support is Apple Silicon (arm64) only; macOS x86-64 is not a supported target${NC}"
            return 1
            ;;
    esac
    build_native "$target_arch"
    stage_macos_launcher "$OUTPUT_FILE"
}

build_linux() {
    local target_arch
    case "$(uname -m)" in
        x86_64|amd64)
            target_arch="x64"
            ;;
        aarch64|arm64)
            target_arch="arm64"
            ;;
        *)
            echo -e "${RED}Error: build_ide.sh currently supports x86_64 and arm64 Linux only${NC}"
            return 1
            ;;
    esac
    build_native "$target_arch"
}

build_native() {
    local target_arch="$1"
    local ide_arg output_arg build_dir_arg
    ide_arg="$(path_for_zanna "$IDE_DIR")"
    output_arg="$(path_for_zanna "$NATIVE_OUTPUT_FILE")"
    build_dir_arg="$(path_for_zanna "$BUILD_DIR")"
    echo -n "  Zanna build (--arch $target_arch)... "
    if ! ZANNA_BUILD_DIR="$build_dir_arg" "$ZANNA" build "$ide_arg" --arch "$target_arch" -o "$output_arg" 2>"$FRONTEND_ERR"; then
        echo -e "${RED}FAILED${NC}"
        head -40 "$FRONTEND_ERR"
        return 1
    fi
    echo -e "${GREEN}OK${NC}"
}

stage_macos_launcher() {
    local launcher_path="$1"
    local launcher_template="$ROOT_DIR/cmake/ZannaStudioMacLauncher.sh"
    mkdir -p "$(dirname "$launcher_path")"
    cp "$launcher_template" "$launcher_path"
    chmod 0755 "$launcher_path"
}

build_info_text() {
    local binary_path="$1"
    local version="unknown"
    if [[ -f "$ROOT_DIR/src/buildmeta/VERSION" ]]; then
        IFS= read -r version <"$ROOT_DIR/src/buildmeta/VERSION" || true
        if [[ -z "$version" ]]; then
            version="unknown"
        fi
    fi
    local timestamp
    timestamp="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    local revision
    if revision="$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null)"; then
        :
    else
        revision="unknown"
    fi
    local dirty=""
    if git -C "$ROOT_DIR" diff --quiet --ignore-submodules -- 2>/dev/null; then
        dirty=""
    else
        dirty=" dirty"
    fi
    printf 'Zanna Studio %s\nBuild: %s\nSource: %s%s\nOutput: %s\nZanna: %s\n' \
        "$version" "$timestamp" "$revision" "$dirty" "$binary_path" "$ZANNA"
}

write_build_info() {
    local binary_path="$1"
    local info_path
    info_path="$(dirname "$binary_path")/zannastudio.buildinfo"
    mkdir -p "$(dirname "$info_path")"
    build_info_text "$binary_path" >"$info_path"
}

mirror_compat_output() {
    if [[ "$SKIP_COMPAT_COPY" == "1" ]]; then
        return 0
    fi
    if [[ "$OUTPUT_FILE" == "$COMPAT_OUTPUT_FILE" ]]; then
        return 0
    fi
    mkdir -p "$(dirname "$COMPAT_OUTPUT_FILE")"
    if [[ "$HOST_SYSTEM" == "Darwin" ]]; then
        if [[ "$NATIVE_OUTPUT_FILE" != "$COMPAT_NATIVE_OUTPUT_FILE" ]]; then
            cp -p "$NATIVE_OUTPUT_FILE" "$COMPAT_NATIVE_OUTPUT_FILE"
        fi
        stage_macos_launcher "$COMPAT_OUTPUT_FILE"
    else
        cp -p "$OUTPUT_FILE" "$COMPAT_OUTPUT_FILE"
    fi
    write_build_info "$COMPAT_NATIVE_OUTPUT_FILE"
    local compat_size
    compat_size=$(ls -lh "$COMPAT_NATIVE_OUTPUT_FILE" | awk '{print $5}')
    echo -e "${GREEN}Compatibility copy: $COMPAT_OUTPUT_FILE ($compat_size)${NC}"
}

echo -e "${CYAN}Building Zanna Studio${NC}"
echo "Source: $IDE_DIR"
echo "Output: $OUTPUT_FILE"
echo "=============================================="

case "$HOST_SYSTEM" in
    Darwin)
        build_macos
        ;;
    Linux)
        build_linux
        ;;
    *)
        echo -e "${RED}Error: unsupported platform${NC}"
        exit 1
        ;;
esac

size=$(ls -lh "$NATIVE_OUTPUT_FILE" | awk '{print $5}')
write_build_info "$NATIVE_OUTPUT_FILE"
mirror_compat_output
echo -e "${GREEN}Built: $OUTPUT_FILE ($size)${NC}"
if [[ "$NATIVE_OUTPUT_FILE" != "$OUTPUT_FILE" ]]; then
    echo "Native payload: $NATIVE_OUTPUT_FILE"
fi
echo "Build info: $(dirname "$OUTPUT_FILE")/zannastudio.buildinfo"
