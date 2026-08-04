#!/usr/bin/env bash
# Script: check_demo_projects.sh
# Purpose: Validate the curated, cross-platform demo project manifest.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")"/.. && pwd)"
MANIFEST="${ZANNA_DEMO_MANIFEST:-${ROOT_DIR}/scripts/demo_projects.list}"
# ZANNA_DEMO_ROOT lets an external demo tree (the zannademos repo) reuse this
# audit unchanged; the default keeps auditing the in-repo examples tree.
DEMO_ROOT="${ZANNA_DEMO_ROOT:-${ROOT_DIR}/examples}"

if [[ ! -f "$MANIFEST" ]]; then
    echo "error: demo project manifest not found: $MANIFEST" >&2
    exit 1
fi

entry_count=0
game_count=0
app_count=0
line_number=0
seen_names=$'\n'

while IFS='|' read -r name category directory extra || [[ -n "${name:-}" ]]; do
    line_number=$((line_number + 1))
    [[ -z "${name:-}" || "${name:0:1}" == "#" ]] && continue

    if [[ -z "${category:-}" || -z "${directory:-}" || -n "${extra:-}" ]]; then
        echo "error: invalid demo project entry at $MANIFEST:$line_number" >&2
        exit 1
    fi
    if [[ ! "$name" =~ ^[a-z0-9][a-z0-9_-]*$ ||
          ! "$directory" =~ ^[a-z0-9][a-z0-9_-]*$ ]]; then
        echo "error: invalid demo project name or directory at $MANIFEST:$line_number" >&2
        exit 1
    fi

    case "$category" in
        games) game_count=$((game_count + 1)) ;;
        # 3D reference demos count as games for the games-vs-apps ratio gate.
        3d) game_count=$((game_count + 1)) ;;
        apps) app_count=$((app_count + 1)) ;;
        *)
            echo "error: invalid demo project category '$category' at $MANIFEST:$line_number" >&2
            exit 1
            ;;
    esac

    case "$seen_names" in
        *$'\n'"$name"$'\n'*)
            echo "error: duplicate demo project name '$name' at $MANIFEST:$line_number" >&2
            exit 1
            ;;
    esac
    seen_names+="$name"$'\n'

    project_file="$DEMO_ROOT/$category/$directory/zanna.project"
    if [[ ! -f "$project_file" ]]; then
        echo "error: demo project file not found: $project_file" >&2
        exit 1
    fi

    language=""
    while read -r key value _rest; do
        if [[ "$key" == "lang" ]]; then
            language="$value"
            break
        fi
    done < "$project_file"
    if [[ "$language" != "zia" ]]; then
        echo "error: demo project '$name' must declare 'lang zia' (found '${language:-missing}')" >&2
        exit 1
    fi

    entry_count=$((entry_count + 1))
done < "$MANIFEST"

if [[ $entry_count -eq 0 ]]; then
    echo "error: demo project manifest contains no projects: $MANIFEST" >&2
    exit 1
fi
if [[ $game_count -le $app_count ]]; then
    echo "error: demo project manifest must contain more games than applications" >&2
    exit 1
fi

echo "demo project manifest audit: $entry_count Zia projects ($game_count games, $app_count apps)"
