#!/usr/bin/env bash
# Run a short host-capability smoke slice after a successful build.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build"
CONFIG="${ZANNA_BUILD_TYPE:-Debug}"

usage() {
    echo "usage: $0 [--build-dir <dir>] [--config <config>]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --build-dir=*)
            BUILD_DIR="${1#--build-dir=}"
            shift
            ;;
        --config)
            CONFIG="$2"
            shift 2
            ;;
        --config=*)
            CONFIG="${1#--config=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

normalize_build_dir_for_shell() {
    local path="$1"
    case "$path" in
        [A-Za-z]:\\*|[A-Za-z]:/*)
            if command -v wslpath >/dev/null 2>&1 &&
               grep -qi microsoft /proc/version 2>/dev/null; then
                wslpath -u "$path"
                return
            fi
            if command -v cygpath >/dev/null 2>&1; then
                cygpath -u "$path"
                return
            fi
            ;;
    esac
    printf '%s\n' "$path"
}

BUILD_DIR="$(normalize_build_dir_for_shell "$BUILD_DIR")"

CAP_FILE="$BUILD_DIR/generated/zanna/platform/Capabilities.hpp"
if [[ ! -f "$CAP_FILE" ]]; then
    echo "error: capability header not found: $CAP_FILE" >&2
    exit 1
fi

cap_value() {
    local macro="$1"
    local value
    value="$(awk -v macro="$macro" '$1 == "#define" && $2 == macro { print $3; exit }' "$CAP_FILE" | tr -d '\r')"
    if [[ -z "$value" ]]; then
        echo 0
    else
        echo "$value"
    fi
}

BUILD_DIR_FOR_CTEST="$BUILD_DIR"
CTEST_CMD="ctest"
CTEST_CONFIG_ARGS=()
if command -v wslpath >/dev/null 2>&1 && grep -qi microsoft /proc/version 2>/dev/null; then
    if command -v ctest.exe >/dev/null 2>&1; then
        CTEST_CMD="ctest.exe"
        BUILD_DIR_FOR_CTEST="$(wslpath -w "$BUILD_DIR")"
    fi
fi
if [[ -n "$CONFIG" ]]; then
    CTEST_CONFIG_ARGS=(-C "$CONFIG")
fi

HOST_WINDOWS="$(cap_value ZANNA_HOST_WINDOWS)"
HOST_MACOS="$(cap_value ZANNA_HOST_MACOS)"
HOST_LINUX="$(cap_value ZANNA_HOST_LINUX)"
HAS_GRAPHICS="$(cap_value ZANNA_BUILD_HAS_GRAPHICS)"
HAS_AUDIO="$(cap_value ZANNA_BUILD_HAS_AUDIO)"
HAS_GUI="$(cap_value ZANNA_BUILD_HAS_GUI)"
NATIVE_LINK_X64="$(cap_value ZANNA_BUILD_NATIVE_LINK_X86_64)"
NATIVE_LINK_A64="$(cap_value ZANNA_BUILD_NATIVE_LINK_AARCH64)"

HAS_DISPLAY=0
if [[ "${ZANNA_SMOKE_FORCE_DISPLAY:-0}" == "1" ]]; then
    HAS_DISPLAY=1
elif [[ $HOST_LINUX -eq 1 ]]; then
    if [[ -n "${DISPLAY:-}" || -n "${WAYLAND_DISPLAY:-}" ]]; then
        HAS_DISPLAY=1
    fi
elif [[ $HOST_MACOS -eq 1 || $HOST_WINDOWS -eq 1 ]]; then
    HAS_DISPLAY=1
fi

echo "=========================================="
echo " Zanna Host Smoke Slice"
echo "=========================================="
if [[ $HOST_WINDOWS -eq 1 ]]; then
    echo " Host:                 Windows"
elif [[ $HOST_MACOS -eq 1 ]]; then
    echo " Host:                 macOS"
elif [[ $HOST_LINUX -eq 1 ]]; then
    echo " Host:                 Linux"
else
    echo " Host:                 Unknown"
fi
echo " Graphics:             $HAS_GRAPHICS"
echo " Audio:                $HAS_AUDIO"
echo " GUI:                  $HAS_GUI"
echo " Display available:    $HAS_DISPLAY"
echo " Native link x86_64:   $NATIVE_LINK_X64"
echo " Native link AArch64:  $NATIVE_LINK_A64"
echo "=========================================="

run_named_tests() {
    local regex="$1"
    local listing
    local detail
    local test_name
    if [[ -z "$regex" ]]; then
        return
    fi
    listing="$("$CTEST_CMD" --test-dir "$BUILD_DIR_FOR_CTEST" "${CTEST_CONFIG_ARGS[@]}" -N -R "$regex" 2>&1 || true)"
    if ! printf '%s\n' "$listing" | grep -q "Test #"; then
        return
    fi
    while IFS= read -r line; do
        if [[ "$line" =~ Test[[:space:]]+\#[0-9]+:[[:space:]]+([^[:space:]]+) ]]; then
            test_name="${BASH_REMATCH[1]}"
            detail="$("$CTEST_CMD" --test-dir "$BUILD_DIR_FOR_CTEST" "${CTEST_CONFIG_ARGS[@]}" -N -V -R "^${test_name}$" 2>&1 || true)"
            if printf '%s\n' "$detail" | grep -q "Could not find executable"; then
                echo "Skipping $test_name because its executable is missing in this build tree"
                continue
            fi
            if ! printf '%s\n' "$detail" | grep -Eq "Test command: .+"; then
                echo "Skipping $test_name because CTest has no runnable command for it"
                continue
            fi
            "$CTEST_CMD" --test-dir "$BUILD_DIR_FOR_CTEST" "${CTEST_CONFIG_ARGS[@]}" --output-on-failure -R "^${test_name}$"
        fi
    done <<< "$listing"
}

core_regex='^(smoke_term_basic|smoke_basic_oop|zia_smoke_paint|zia_smoke_crackman|zia_smoke_chess)$'
run_named_tests "$core_regex"

surface_link_regex='^(test_rt_graphics_surface_link|test_rt_audio_surface_link)$'
run_named_tests "$surface_link_regex"

if [[ $HAS_GRAPHICS -eq 0 ]]; then
    run_named_tests '^test_rt_canvas_unavailable$'
else
    echo "Skipping no-graphics canvas unavailable test on graphics-enabled build"
fi

if [[ $HAS_AUDIO -eq 0 ]]; then
    run_named_tests '^test_rt_audio_unavailable$'
else
    echo "Skipping no-audio unavailable test on audio-enabled build"
fi

planner_regex='^(test_linker_platform_import_planners|test_linker_runtime_import_audit|test_linker_elf_exe_writer)$'
run_named_tests "$planner_regex"

if [[ $HOST_MACOS -eq 1 && $NATIVE_LINK_A64 -eq 1 ]]; then
    native_link_regex='^(native_smoke_chess_ai_arm64|native_smoke_crackman_movement_arm64|native_smoke_zannastudio_completion_arm64)$'
    run_named_tests "$native_link_regex"
fi

if [[ $HAS_GRAPHICS -eq 1 && $HAS_DISPLAY -eq 1 ]]; then
    display_regex='^(zia_smoke_zannastudio)$'
    run_named_tests "$display_regex"
else
    echo "Skipping display-bound smoke tests on this host"
fi
