#!/bin/bash
#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/build_demos_mac.sh
# Purpose: Build curated macOS ARM64 demos with Zanna's native toolchain.
# Key invariants: Every frontend and backend stage uses O2; packaged assets
#                 remain attached to one-step project builds.
# Ownership/Lifetime: Temporary build artifacts live under /tmp and are removed
#                     after each demo; completed binaries live in examples/bin.
# Links: scripts/demo_projects.list, scripts/build_demos.sh
#
#===----------------------------------------------------------------------===#

# Build curated Zia showcase binaries on macOS arm64 using the native assembler
# and linker. Optional smoke-run validation can be enabled with --run. This uses
# zero external tools for the build path — no system assembler (cc -c), no
# system linker (cc/ld).
# Usage: ./scripts/build_demos_mac.sh [--clean] [--run|--skip-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build"
BIN_DIR="$ROOT_DIR/examples/bin"
DEMO_MANIFEST="$SCRIPT_DIR/demo_projects.list"

ZANNA="$BUILD_DIR/src/tools/zanna/zanna"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

usage() {
    echo "Usage: $0 [--clean] [--run|--skip-run]"
    echo "  --clean      Remove existing binaries before building"
    echo "  --run        Launch each built demo for smoke validation"
    echo "  --skip-run   Build only; skip launch validation (default)"
    exit 1
}

CLEAN=0
SKIP_RUN=1
for arg in "$@"; do
    case $arg in
        --clean)
            CLEAN=1
            ;;
        --run)
            SKIP_RUN=0
            ;;
        --skip-run)
            SKIP_RUN=1
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown argument: $arg"
            usage
            ;;
    esac
done

if [[ ! -x "$ZANNA" ]]; then
    echo -e "${RED}Error: zanna tool not found at $ZANNA${NC}"
    echo "Run './scripts/build_zanna_mac.sh' first"
    exit 1
fi

mkdir -p "$BIN_DIR"

if [[ $CLEAN -eq 1 ]]; then
    echo "Cleaning existing binaries..."
    rm -f "$BIN_DIR"/*
fi

RUN_TIMEOUT_DEFAULT="${ZANNA_DEMO_TIMEOUT:-5}"
RUN_DEMO_RC=0

SHOWCASE_DEMOS=()
load_demo_manifest() {
    if [[ ! -f "$DEMO_MANIFEST" ]]; then
        echo -e "${RED}Error: demo manifest not found: $DEMO_MANIFEST${NC}"
        exit 1
    fi

    local name category directory extra
    local line_number=0
    while IFS='|' read -r name category directory extra || [[ -n "${name:-}" ]]; do
        line_number=$((line_number + 1))
        [[ -z "${name:-}" || "${name:0:1}" == "#" ]] && continue
        if [[ -z "${category:-}" || -z "${directory:-}" || -n "${extra:-}" ]]; then
            echo -e "${RED}Error: invalid demo manifest entry at line $line_number${NC}"
            exit 1
        fi
        case "$category" in
            # 3d carries the reference demos under examples/3d (they count as
            # games in check_demo_projects.sh's ratio gate).
            games|apps|3d) ;;
            *)
                echo -e "${RED}Error: invalid demo category '$category' at line $line_number${NC}"
                exit 1
                ;;
        esac
        SHOWCASE_DEMOS+=("$name:$ROOT_DIR/examples/$category/$directory")
    done < "$DEMO_MANIFEST"

    if [[ ${#SHOWCASE_DEMOS[@]} -eq 0 ]]; then
        echo -e "${RED}Error: demo manifest contains no projects${NC}"
        exit 1
    fi
}

load_demo_manifest

snapshot_bin_dir() {
    find "$BIN_DIR" -mindepth 1 -maxdepth 1 -print | sed 's#.*/##' | LC_ALL=C sort
}

cleanup_run_artifacts() {
    local before_list="$1"
    local after_list="$2"
    local keep_entry="$3"

    while IFS= read -r entry; do
        if [[ "$entry" == "$keep_entry" ]]; then
            continue
        fi
        rm -rf -- "$BIN_DIR/$entry"
    done < <(comm -13 "$before_list" "$after_list")
}

run_demo() {
    local name="$1"
    local exe_file="$2"
    local run_out="$3"
    local run_err="$4"
    local timeout_secs="$RUN_TIMEOUT_DEFAULT"
    RUN_DEMO_RC=0

    if [[ $SKIP_RUN -eq 1 ]]; then
        return 0
    fi

    case "$name" in
        3dbowling|ridgebound|zannasql|xenoscape)
            timeout_secs=10
            ;;
    esac

    local before_list="${run_out}.before"
    local after_list="${run_out}.after"
    snapshot_bin_dir >"$before_list"

    local rc=0
    (
        cd "$BIN_DIR"
        "./$(basename "$exe_file")" >"$run_out" 2>"$run_err" &
        local pid=$!

        sleep "$timeout_secs"
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" >/dev/null 2>&1 || true
            exit 124
        fi

        wait "$pid"
    ) || rc=$?

    snapshot_bin_dir >"$after_list"
    cleanup_run_artifacts "$before_list" "$after_list" "$(basename "$exe_file")"
    rm -f "$before_list" "$after_list"

    if [[ $rc -eq 0 || $rc -eq 124 ]]; then
        RUN_DEMO_RC=$rc
        return 0
    fi

    RUN_DEMO_RC=$rc
    return 1
}

build_demo() {
    local name="$1"
    local project_dir="$2"
    local exe_file="$BIN_DIR/${name}"
    local tmp_base="/tmp/zanna_demo_${name}_$$"
    local il_file="${tmp_base}.il"
    local frontend_err="${tmp_base}.front.err"
    local codegen_err="${tmp_base}.codegen.err"
    local run_out="${tmp_base}.run.out"
    local run_err="${tmp_base}.run.err"

    if [[ ! -f "$project_dir/zanna.project" ]]; then
        echo -e "${RED}  Error: No zanna.project found in $project_dir${NC}"
        return 1
    fi

    # All demos build at -O2. The loop-rotate SSA-reconstruction and inliner
    # escaped-param typing bugs that previously forced centipede/chess/
    # crackman down to -O0 have been fixed in the IL optimizer.
    local codegen_opt="-O2"

    # Projects that bake assets into the build (embed -> .rodata, pack -> a .zpak
    # beside the binary) must build in one step: `zanna build` runs the asset
    # compiler, whereas the two-step IL->codegen path carries no assets (the
    # binary would silently fall back to loose files on disk). Plain `asset`
    # directives ship external files and load fine via the two-step, so they are
    # not matched here. Both paths use the from-scratch native asm + linker.
    if grep -qE '^[[:space:]]*(embed|pack|pack-compressed)[[:space:]]' "$project_dir/zanna.project"; then
        echo -n "  Build+embed -> native (asm+link)... "
        if ! "$ZANNA" build "$project_dir" --arch arm64 "$codegen_opt" -o "$exe_file" 2>"$codegen_err"; then
            echo -e "${RED}FAILED${NC}"
            head -20 "$codegen_err"
            rm -f "$frontend_err" "$codegen_err" "$run_out" "$run_err" "$il_file"
            return 1
        fi
        echo -e "${GREEN}OK${NC}"
    else
        # Step 1: Frontend — compile to IL.
        echo -n "  Frontend -> IL... "
        if ! "$ZANNA" build "$project_dir" "$codegen_opt" -o "$il_file" 2>"$frontend_err"; then
            echo -e "${RED}FAILED${NC}"
            head -20 "$frontend_err"
            rm -f "$frontend_err" "$codegen_err" "$run_out" "$run_err" "$il_file"
            return 1
        fi
        echo -e "${GREEN}OK${NC}"

        # Step 2: Native codegen — binary encoder + native linker.
        echo -n "  Codegen (native asm+link)... "
        if ! "$ZANNA" codegen arm64 "$il_file" --native-asm --native-link \
            --skip-il-optimization "$codegen_opt" -o "$exe_file" 2>"$codegen_err"; then
            echo -e "${RED}FAILED${NC}"
            head -20 "$codegen_err"
            rm -f "$frontend_err" "$codegen_err" "$run_out" "$run_err" "$il_file"
            return 1
        fi
        echo -e "${GREEN}OK${NC}"
    fi

    if [[ $SKIP_RUN -eq 0 ]]; then
        echo -n "  Run smoke... "
        if ! run_demo "$name" "$exe_file" "$run_out" "$run_err"; then
            echo -e "${RED}FAILED${NC}"
            echo "  Exit code: $RUN_DEMO_RC"
            if [[ -s "$run_out" ]]; then
                echo "  stdout:"
                head -20 "$run_out"
            fi
            if [[ -s "$run_err" ]]; then
                echo "  stderr:"
                head -20 "$run_err"
            fi
            rm -f "$frontend_err" "$codegen_err" "$run_out" "$run_err" "$il_file"
            return 1
        fi
        echo -e "${GREEN}OK${NC}"
    fi

    rm -f "$frontend_err" "$codegen_err" "$run_out" "$run_err" "$il_file"

    local size=$(ls -lh "$exe_file" | awk '{print $5}')
    echo -e "  ${GREEN}Built: $exe_file ($size)${NC}"
    return 0
}

echo -e "${CYAN}Building Zanna demos with native assembler + linker (arm64)${NC}"
if [[ $SKIP_RUN -eq 0 ]]; then
    echo -e "${CYAN}Run validation: launch from ./examples/bin with timeout=${RUN_TIMEOUT_DEFAULT}s${NC}"
fi
echo "=============================================="
echo ""

FAILED=0
SUCCEEDED=0
SKIPPED=0

echo "=== Zia Showcase Demos ==="
echo ""

for demo in "${SHOWCASE_DEMOS[@]}"; do
    IFS=':' read -r name project_dir <<< "$demo"
    if [[ ! -d "$project_dir" ]]; then
        echo -e "Skipping $name (${YELLOW}directory not found${NC})"
        SKIPPED=$((SKIPPED + 1))
        echo ""
        continue
    fi
    echo "Building $name..."
    if build_demo "$name" "$project_dir"; then
        SUCCEEDED=$((SUCCEEDED + 1))
    else
        FAILED=$((FAILED + 1))
    fi
    echo ""
done

echo "=============================================="
TOTAL=$((SUCCEEDED + FAILED + SKIPPED))
echo -e "Results: ${GREEN}$SUCCEEDED passed${NC}, ${RED}$FAILED failed${NC}, ${YELLOW}$SKIPPED skipped${NC} (of $TOTAL)"

if [[ $FAILED -eq 0 ]]; then
    echo ""
    echo "Binaries are in: $BIN_DIR"
    ls -lh "$BIN_DIR"
else
    exit 1
fi
