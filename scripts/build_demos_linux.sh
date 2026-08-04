#!/bin/bash
# Build curated Zia showcase binaries on Linux using Zanna's native backend and
# selected linker. Optional smoke-run validation can be enabled with --run.
# Usage: ./scripts/build_demos_linux.sh [options]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build"
# ZANNA_DEMO_ROOT / ZANNA_DEMO_BIN_DIR / ZANNA_DEMO_MANIFEST let an external
# demo tree (the zannademos repo) reuse this builder unchanged.
DEMO_ROOT="${ZANNA_DEMO_ROOT:-$ROOT_DIR/examples}"
BIN_DIR="${ZANNA_DEMO_BIN_DIR:-$ROOT_DIR/examples/bin}"
DEMO_MANIFEST="${ZANNA_DEMO_MANIFEST:-$SCRIPT_DIR/demo_projects.list}"

ZANNA="$BUILD_DIR/src/tools/zanna/zanna"
LINK_CMD="${CXX:-c++}"
RUN_TIMEOUT_DEFAULT="${ZANNA_DEMO_TIMEOUT:-5}"
RUN_DEMO_RC=0
STAMP_DIR="$BIN_DIR/.demo-stamps"
DEMO_OPT="${ZANNA_DEMO_OPT:-O1}"
DEMO_JOBS="${ZANNA_DEMO_JOBS:-}"
LINK_MODE="${ZANNA_DEMO_LINKER:-native}"
TIMINGS=0
FORCE_REBUILD=0

RUNTIME_ARCHIVE="$BUILD_DIR/src/runtime/libzanna_runtime.a"
GUI_LIB="$BUILD_DIR/src/lib/gui/libzannagui.a"
GFX_LIB="$BUILD_DIR/lib/libzannagfx.a"
AUDIO_LIB="$BUILD_DIR/lib/libzannaaud.a"
GRAPHICS_SYSTEM_LIBS=()

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

usage() {
    echo "Usage: $0 [--clean] [--run|--skip-run] [--release|--opt O1|O2]"
    echo "          [--jobs N] [--linker native|system] [--timings] [--rebuild]"
    echo "  --clean      Remove existing binaries before building"
    echo "  --run        Launch each built demo for smoke validation"
    echo "  --skip-run   Build only; skip launch validation (default)"
    echo "  --release    Build demos at O2 (interactive builds default to O1)"
    echo "  --opt LEVEL  Select O1 or O2 explicitly"
    echo "  --jobs N     Build up to N demos concurrently"
    echo "  --linker     Select native (default) or legacy system linker"
    echo "  --timings    Print frontend, backend, and native-link stage timings"
    echo "  --rebuild    Ignore dependency stamps without deleting binaries"
    exit 1
}

CLEAN=0
SKIP_RUN=1
while [[ $# -gt 0 ]]; do
    arg="$1"
    case "$arg" in
        --clean)
            CLEAN=1
            ;;
        --run)
            SKIP_RUN=0
            ;;
        --skip-run)
            SKIP_RUN=1
            ;;
        --release)
            DEMO_OPT="O2"
            ;;
        --opt)
            shift
            [[ $# -gt 0 ]] || usage
            DEMO_OPT="$1"
            ;;
        --jobs)
            shift
            [[ $# -gt 0 ]] || usage
            DEMO_JOBS="$1"
            ;;
        --linker)
            shift
            [[ $# -gt 0 ]] || usage
            LINK_MODE="$1"
            ;;
        --timings)
            TIMINGS=1
            ;;
        --rebuild)
            FORCE_REBUILD=1
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown argument: $arg"
            usage
            ;;
    esac
    shift
done

if [[ "$DEMO_OPT" != "O1" && "$DEMO_OPT" != "O2" ]]; then
    echo -e "${RED}Error: --opt must be O1 or O2${NC}"
    exit 1
fi
if [[ "$LINK_MODE" != "native" && "$LINK_MODE" != "system" ]]; then
    echo -e "${RED}Error: --linker must be native or system${NC}"
    exit 1
fi
if [[ -z "$DEMO_JOBS" ]]; then
    DEMO_JOBS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
fi
if [[ ! "$DEMO_JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo -e "${RED}Error: --jobs must be a positive integer${NC}"
    exit 1
fi
HOST_CPUS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
THREADS_PER_DEMO=$((HOST_CPUS / DEMO_JOBS))
if [[ $THREADS_PER_DEMO -lt 1 ]]; then
    THREADS_PER_DEMO=1
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    echo -e "${RED}Error: build_demos_linux.sh must be run on Linux${NC}"
    exit 1
fi

case "$(uname -m)" in
    x86_64|amd64)
        TARGET_ARCH="x64"
        NATIVE_LINK=0
        ;;
    aarch64|arm64)
        TARGET_ARCH="arm64"
        NATIVE_LINK=1
        ;;
    *)
        echo -e "${RED}Error: build_demos_linux.sh currently supports x86_64 and arm64 Linux only${NC}"
        exit 1
        ;;
esac

if [[ ! -x "$ZANNA" ]]; then
    echo -e "${RED}Error: zanna tool not found at $ZANNA${NC}"
    echo "Run './scripts/build_zanna_linux.sh' first"
    exit 1
fi

if [[ "$LINK_MODE" == "system" ]] && ! command -v "$LINK_CMD" >/dev/null 2>&1; then
    echo -e "${RED}Error: C++ linker '$LINK_CMD' not found${NC}"
    exit 1
fi

if [[ $SKIP_RUN -eq 0 ]] && ! command -v timeout >/dev/null 2>&1; then
    echo -e "${RED}Error: 'timeout' command not found${NC}"
    exit 1
fi

# Smoke runs create and remove files in the shared examples/bin directory.
# Keep that validation deterministic and avoid multiple graphical demos
# competing for the same display/audio devices.
if [[ $SKIP_RUN -eq 0 ]]; then
    DEMO_JOBS=1
    THREADS_PER_DEMO=$HOST_CPUS
fi

for required in "$RUNTIME_ARCHIVE" "$GUI_LIB" "$GFX_LIB" "$AUDIO_LIB"; do
    if [[ ! -f "$required" ]]; then
        echo -e "${RED}Error: required library not found: $required${NC}"
        echo "Run './scripts/build_zanna_linux.sh' first"
        exit 1
    fi
done

# Explicit Wayland and AUTO-without-X11 builds have no Xlib symbols. Normal AUTO and explicit X11
# builds retain the link because their static graphics archive contains the X11 adapter.
if grep -qE '^ZANNAGFX_BACKEND:INTERNAL=X11$|^ZANNAGFX_AUTO_HAS_X11:INTERNAL=ON$' \
        "$BUILD_DIR/CMakeCache.txt"; then
    GRAPHICS_SYSTEM_LIBS=(-lX11)
fi

mkdir -p "$BIN_DIR" "$STAMP_DIR"

if [[ $CLEAN -eq 1 ]]; then
    echo "Cleaning existing binaries..."
    rm -f "$BIN_DIR"/*
    rm -rf "$STAMP_DIR"
    mkdir -p "$STAMP_DIR"
fi

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
        SHOWCASE_DEMOS+=("$name:$DEMO_ROOT/$category/$directory")
    done < "$DEMO_MANIFEST"

    if [[ ${#SHOWCASE_DEMOS[@]} -eq 0 ]]; then
        echo -e "${RED}Error: demo manifest contains no projects${NC}"
        exit 1
    fi
}

load_demo_manifest

link_demo() {
    local obj_file="$1"
    local exe_file="$2"
    local err_file="$3"

    if ! "$LINK_CMD" \
        "$obj_file" \
        "$RUNTIME_ARCHIVE" \
        "$GUI_LIB" \
        "$GFX_LIB" \
        "$AUDIO_LIB" \
        "${GRAPHICS_SYSTEM_LIBS[@]}" \
        -lasound \
        -pthread \
        -lm \
        -ldl \
        -o "$exe_file" >"$err_file" 2>&1; then
        return 1
    fi

    return 0
}

demo_stamp_value() {
    local name="$1"
    printf '%s\n' "arch=$TARGET_ARCH opt=$DEMO_OPT linker=$LINK_MODE name=$name"
}

is_demo_up_to_date() {
    local name="$1"
    local project_dir="$2"
    local exe_file="$BIN_DIR/$name"
    local stamp_file="$STAMP_DIR/$name.stamp"

    [[ $FORCE_REBUILD -eq 0 && -x "$exe_file" && -f "$stamp_file" ]] || return 1
    [[ "$(<"$stamp_file")" == "$(demo_stamp_value "$name")" ]] || return 1
    [[ "$ZANNA" -ot "$exe_file" ]] || return 1
    if find "$project_dir" -type f -newer "$exe_file" -print -quit | grep -q .; then
        return 1
    fi
    if find "$BUILD_DIR/src/runtime" "$BUILD_DIR/src/lib" "$BUILD_DIR/lib" \
        -type f \( -name '*.a' -o -name '*.so' \) -newer "$exe_file" -print -quit 2>/dev/null | \
        grep -q .; then
        return 1
    fi
    return 0
}

write_demo_stamp() {
    local name="$1"
    demo_stamp_value "$name" >"$STAMP_DIR/$name.stamp"
}

snapshot_bin_dir() {
    find "$BIN_DIR" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort
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
        timeout "$timeout_secs" "./$(basename "$exe_file")"
    ) >"$run_out" 2>"$run_err" || rc=$?

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
    local obj_file="${tmp_base}.o"
    local frontend_err="${tmp_base}.front.err"
    local codegen_out="${tmp_base}.codegen.out"
    local codegen_err="${tmp_base}.codegen.err"
    local link_err="${tmp_base}.link.err"
    local run_out="${tmp_base}.run.out"
    local run_err="${tmp_base}.run.err"

    if [[ ! -f "$project_dir/zanna.project" ]]; then
        echo -e "${RED}  Error: No zanna.project found in $project_dir${NC}"
        return 1
    fi

    if is_demo_up_to_date "$name" "$project_dir"; then
        echo -e "  ${GREEN}Up to date: $exe_file${NC}"
        return 2
    fi

    # Every demo uses the requested common optimization level. The
    # loop-rotate SSA-reconstruction and inliner escaped-param typing bugs that
    # previously forced a few arm64 demos down to -O0 have been fixed in the
    # target-independent IL optimizer.
    local demo_opt="-$DEMO_OPT"
    local timing_args=()
    local link_args=()
    if [[ $TIMINGS -eq 1 ]]; then
        timing_args+=(--time-compile)
    fi
    if [[ "$DEMO_OPT" == "O1" ]]; then
        link_args+=(--fast-link)
    fi

    if [[ "$LINK_MODE" == "native" ]] || \
        grep -qE '^[[:space:]]*(embed|pack|pack-compressed)[[:space:]]' "$project_dir/zanna.project"; then
        echo -n "  Build -> native (in-memory IL)... "
        if ! ZANNA_LINKER_STATS="$TIMINGS" "$ZANNA" build "$project_dir" --arch "$TARGET_ARCH" \
            "$demo_opt" "${timing_args[@]}" "${link_args[@]}" -o "$exe_file" \
            >"$codegen_out" 2>"$codegen_err"; then
            echo -e "${RED}FAILED${NC}"
            head -20 "$codegen_err"
            rm -f "$il_file" "$obj_file" "$frontend_err" "$codegen_out" "$codegen_err" \
                "$link_err" "$run_out" "$run_err"
            return 1
        fi
        if [[ $TIMINGS -eq 1 ]]; then
            grep -E '^\[(time-compile|link-time)\]' "$codegen_err" || true
        fi
        echo -e "${GREEN}OK${NC}"
    else
        echo -n "  Frontend -> IL... "
        if ! "$ZANNA" build "$project_dir" "$demo_opt" "${timing_args[@]}" -o "$il_file" \
            2>"$frontend_err"; then
            echo -e "${RED}FAILED${NC}"
            head -20 "$frontend_err"
            rm -f "$il_file" "$obj_file" "$frontend_err" "$codegen_out" "$codegen_err" \
                "$link_err" "$run_out" "$run_err"
            return 1
        fi
        echo -e "${GREEN}OK${NC}"

        if [[ $NATIVE_LINK -eq 1 ]]; then
            echo -n "  Codegen (native asm+link)... "
            if ! "$ZANNA" codegen "$TARGET_ARCH" "$il_file" --native-asm --native-link \
                --skip-il-optimization "$demo_opt" -o "$exe_file" \
                >"$codegen_out" 2>"$codegen_err"; then
                echo -e "${RED}FAILED${NC}"
                head -20 "$codegen_err"
                rm -f "$il_file" "$obj_file" "$frontend_err" "$codegen_out" "$codegen_err" \
                    "$link_err" "$run_out" "$run_err"
                return 1
            fi
        else
            echo -n "  Codegen (native obj)... "
            if ! "$ZANNA" codegen "$TARGET_ARCH" compile "$il_file" --native-asm \
                --skip-il-optimization "$demo_opt" -o "$obj_file" \
                >"$codegen_out" 2>"$codegen_err"; then
                echo -e "${RED}FAILED${NC}"
                head -20 "$codegen_err"
                rm -f "$il_file" "$obj_file" "$frontend_err" "$codegen_out" "$codegen_err" \
                    "$link_err" "$run_out" "$run_err"
                return 1
            fi
            echo -e "${GREEN}OK${NC}"

            echo -n "  Link (system c++)... "
            if ! link_demo "$obj_file" "$exe_file" "$link_err"; then
                echo -e "${RED}FAILED${NC}"
                head -40 "$link_err"
                rm -f "$il_file" "$obj_file" "$frontend_err" "$codegen_out" "$codegen_err" \
                    "$link_err" "$run_out" "$run_err"
                return 1
            fi
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
            rm -f "$il_file" "$obj_file" "$frontend_err" "$codegen_out" "$codegen_err" \
                "$link_err" "$run_out" "$run_err"
            return 1
        fi
        echo -e "${GREEN}OK${NC}"
    fi

    rm -f "$il_file" "$obj_file" "$frontend_err" "$codegen_out" "$codegen_err" \
        "$link_err" "$run_out" "$run_err"

    write_demo_stamp "$name"

    local size
    size=$(ls -lh "$exe_file" | awk '{print $5}')
    echo -e "  ${GREEN}Built: $exe_file ($size)${NC}"
    return 0
}

echo -e "${CYAN}Building Zanna demos on Linux (${TARGET_ARCH})${NC}"
if [[ "$LINK_MODE" == "native" ]]; then
    echo -e "${CYAN}Object generation: Zanna native backend${NC}"
    echo -e "${CYAN}Final link: Zanna native linker${NC}"
else
    echo -e "${CYAN}Object generation: Zanna native backend${NC}"
    echo -e "${CYAN}Final link: system c++${NC}"
fi
echo -e "${CYAN}Optimization: ${DEMO_OPT}; concurrent demos: ${DEMO_JOBS}${NC}"
if [[ $SKIP_RUN -eq 0 ]]; then
    echo -e "${CYAN}Run validation: launch from $BIN_DIR with timeout=${RUN_TIMEOUT_DEFAULT}s${NC}"
fi
echo "=============================================="
echo ""

FAILED=0
SUCCEEDED=0
SKIPPED=0

echo "=== Zia Showcase Demos ==="
echo ""

build_demo_group() {
    local array_name="$1"
    local -n demos_ref="$array_name"
    local index=0

    while [[ $index -lt ${#demos_ref[@]} ]]; do
        local pids=()
        local logs=()
        local statuses=()
        local names=()
        local launched=0
        while [[ $index -lt ${#demos_ref[@]} && $launched -lt $DEMO_JOBS ]]; do
            local demo="${demos_ref[$index]}"
            local name project_dir
            IFS=':' read -r name project_dir <<< "$demo"
            local job_base="/tmp/zanna_demo_job_${name}_$$"
            local log_file="${job_base}.log"
            local status_file="${job_base}.status"
            (
                export ZANNA_CODEGEN_THREADS="${ZANNA_CODEGEN_THREADS:-$THREADS_PER_DEMO}"
                export ZANNA_OPT_THREADS="${ZANNA_OPT_THREADS:-$THREADS_PER_DEMO}"
                if [[ ! -d "$project_dir" ]]; then
                    echo -e "Skipping $name (${YELLOW}directory not found${NC})"
                    echo 3 >"$status_file"
                    exit 0
                fi
                echo "Building $name..."
                local rc=0
                build_demo "$name" "$project_dir" || rc=$?
                echo "$rc" >"$status_file"
            ) >"$log_file" 2>&1 &
            pids+=("$!")
            logs+=("$log_file")
            statuses+=("$status_file")
            names+=("$name")
            index=$((index + 1))
            launched=$((launched + 1))
        done

        local job
        for job in "${!pids[@]}"; do
            wait "${pids[$job]}" || true
            cat "${logs[$job]}"
            local rc=1
            if [[ -f "${statuses[$job]}" ]]; then
                rc="$(<"${statuses[$job]}")"
            fi
            case "$rc" in
                0) SUCCEEDED=$((SUCCEEDED + 1)) ;;
                2|3) SKIPPED=$((SKIPPED + 1)) ;;
                *) FAILED=$((FAILED + 1)) ;;
            esac
            rm -f "${logs[$job]}" "${statuses[$job]}"
            echo ""
        done
    done
}

build_demo_group SHOWCASE_DEMOS

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
