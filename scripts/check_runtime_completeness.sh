#!/usr/bin/env bash
#===----------------------------------------------------------------------===//
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===//
#
# File: scripts/check_runtime_completeness.sh
# Purpose: Validate cross-row references in the modular runtime definition set,
#          and check that every registered C symbol keeps a definition in a
#          graphics-disabled runtime build.
# Key invariants:
#   - rtgen is the only parser for runtime definition manifests and fragments.
#   - Every class constructor, property accessor, and method target resolves.
#   - Every RT_FUNC and RT_INTERNAL_FUNC C symbol is defined by a source
#     compiled when ZANNA_ENABLE_GRAPHICS is not defined; definitions inside
#     `#ifdef ZANNA_ENABLE_GRAPHICS` regions do not count.
# Ownership/Lifetime:
#   - Reads repository sources and an existing rtgen build artifact.
#   - Creates no persistent files.
# Links: src/tools/rtgen/rtgen.cpp, src/il/runtime/runtime.def,
#        docs/internals/codemap/runtime-graphics-stubs.md
#
#===----------------------------------------------------------------------===//

set -euo pipefail

readonly DEF="src/il/runtime/runtime.def"
RTGEN="${ZANNA_RTGEN:-build/src/rtgen}"

if [[ ! -f "${DEF}" ]]; then
    echo "ERROR: ${DEF} not found. Run from the project root." >&2
    exit 2
fi

if [[ ! -x "${RTGEN}" ]]; then
    config="${ZANNA_BUILD_TYPE:-Debug}"
    candidate="build/src/${config}/rtgen.exe"
    if [[ -x "${candidate}" ]]; then
        RTGEN="${candidate}"
    else
        echo "ERROR: rtgen is not built. Run the platform build script first." >&2
        exit 2
    fi
fi

"${RTGEN}" --validate "${DEF}"
echo "OK: Runtime definition references are complete."

#===----------------------------------------------------------------------===//
# Graphics-disabled definition parity.
#
# The generated VM handler table takes the address of every RT_FUNC and
# RT_INTERNAL_FUNC C symbol unconditionally, so each one must be defined by a
# source that still compiles when ZANNA_ENABLE_GRAPHICS is OFF. That corpus is
# every always-built runtime component list plus RT_GRAPHICS_DISABLED_SOURCES
# (platform-conditional `list(APPEND ...)` entries included), with one level of
# `.inc` includes.
#
# A symbol counts as defined only when a function definition for it appears
# outside `#ifdef ZANNA_ENABLE_GRAPHICS` regions, either as
# `<type> rt_name(...)` on a line that does not end in `;`, or through a
# `*DEFINE*(rt_name, ...)` macro. This is a textual audit: it cannot see
# compile errors, duplicate definitions, or signature drift, which need a real
# graphics-disabled build (see docs/internals/codemap/runtime-graphics-stubs.md).
#===----------------------------------------------------------------------===//

readonly RT_CMAKE="src/runtime/CMakeLists.txt"
readonly RT_SOURCE_LISTS="BASE|ARRAY|OOP|COLLECTIONS|GAME|TEXT|IO_FS|EXEC|GRAPHICS_DISABLED|AUDIO|THREADS|LOCALIZATION|NETWORK|SERVICES"
parity_tmp="$(mktemp -d)"
trap 'rm -rf "${parity_tmp}"' EXIT

awk '/RT_(INTERNAL_)?FUNC\(/ {
        line = $0
        sub(/.*RT_(INTERNAL_)?FUNC\(/, "", line)
        n = split(line, parts, ",")
        if (n >= 2) {
            s = parts[2]
            gsub(/[ \t]/, "", s)
            if (s ~ /^rt_/)
                print s
        }
    }' $(find src/il/runtime/defs -name '*.def' | sort) |
    sort -u > "${parity_tmp}/def_syms"

awk -v lists="${RT_SOURCE_LISTS}" '
    function emit(token) {
        gsub(/[ \t()]/, "", token)
        if (token ~ /\.(c|m|cpp)$/)
            print "src/runtime/" token
    }
    BEGIN { set_re = "^set\\(RT_(" lists ")_SOURCES"; append_re = "list\\(APPEND RT_(" lists ")_SOURCES" }
    $0 ~ set_re { grab = 1; next }
    grab && /^\)/ { grab = 0; next }
    grab { emit($1); next }
    $0 ~ append_re {
        line = $0
        sub(/.*list\(APPEND RT_[A-Z_]+_SOURCES/, "", line)
        n = split(line, tokens, /[ \t]+/)
        for (i = 1; i <= n; i++)
            emit(tokens[i])
    }' "${RT_CMAKE}" | sort -u > "${parity_tmp}/link_files"

cp "${parity_tmp}/link_files" "${parity_tmp}/corpus_files"
while IFS= read -r src_file; do
    [[ -f "${src_file}" ]] || continue
    src_dir="$(dirname "${src_file}")"
    { grep -h '#include "' "${src_file}" 2>/dev/null || true; } |
        sed -n 's/.*#include "\([^"]*\.inc\)".*/\1/p' |
        while IFS= read -r inc_file; do
            if [[ -f "${src_dir}/${inc_file}" ]]; then
                echo "${src_dir}/${inc_file}"
            fi
        done
done < "${parity_tmp}/link_files" >> "${parity_tmp}/corpus_files"
sort -u "${parity_tmp}/corpus_files" -o "${parity_tmp}/corpus_files"

while IFS= read -r src_file; do
    [[ -f "${src_file}" ]] || continue
    awk '
        function graphics_only(   i) {
            for (i = 1; i <= depth; i++)
                if (state[i] == "G")
                    return 1
            return 0
        }
        /^[ \t]*#[ \t]*if/ {
            depth++
            if ($0 ~ /^[ \t]*#[ \t]*ifdef[ \t]+ZANNA_ENABLE_GRAPHICS([ \t]|$)/ ||
                $0 ~ /^[ \t]*#[ \t]*if[ \t]+defined[ \t]*\(ZANNA_ENABLE_GRAPHICS\)/)
                state[depth] = "G"
            else if ($0 ~ /^[ \t]*#[ \t]*ifndef[ \t]+ZANNA_ENABLE_GRAPHICS([ \t]|$)/)
                state[depth] = "N"
            else
                state[depth] = "O"
            next
        }
        /^[ \t]*#[ \t]*else/ {
            if (state[depth] == "G")
                state[depth] = "N"
            else if (state[depth] == "N")
                state[depth] = "G"
            next
        }
        /^[ \t]*#[ \t]*elif/ {
            if (state[depth] == "G")
                state[depth] = "O"
            next
        }
        /^[ \t]*#[ \t]*endif/ {
            if (depth > 0)
                depth--
            next
        }
        graphics_only() { next }
        /^[ \t]*[A-Z][A-Z0-9_]*DEFINE[A-Z0-9_]*\([ \t]*rt_[a-z0-9_]+[ \t]*[,)]/ {
            line = $0
            sub(/^[^(]*\([ \t]*/, "", line)
            sub(/[ \t]*[,)].*/, "", line)
            print line
            next
        }
        /^[A-Za-z_][A-Za-z0-9_ \t*]*[ \t*]rt_[a-z0-9_]+[ \t]*\(/ && $0 !~ /;[ \t]*$/ {
            line = $0
            sub(/[ \t]*\(.*/, "", line)
            n = split(line, parts, /[ \t*]+/)
            print parts[n]
        }' "${src_file}"
done < "${parity_tmp}/corpus_files" | sort -u > "${parity_tmp}/defined"

if ! comm -23 "${parity_tmp}/def_syms" "${parity_tmp}/defined" > "${parity_tmp}/missing"; then
    echo "ERROR: graphics-disabled parity comparison failed." >&2
    exit 2
fi

if [[ -s "${parity_tmp}/missing" ]]; then
    echo "ERROR: runtime entry points with no definition in graphics-disabled builds:" >&2
    sed 's/^/  /' "${parity_tmp}/missing" >&2
    echo "Add a stub in the src/runtime/graphics/common/*_stubs.c file that owns the class," >&2
    echo "or move a backend-free definition outside #ifdef ZANNA_ENABLE_GRAPHICS in a source" >&2
    echo "listed in RT_GRAPHICS_DISABLED_SOURCES or another always-built list (${RT_CMAKE})." >&2
    exit 1
fi

def_total="$(wc -l < "${parity_tmp}/def_syms" | tr -d ' ')"
echo "OK: graphics-disabled definition parity holds for ${def_total} registered entry points."
