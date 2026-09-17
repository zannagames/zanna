#!/usr/bin/env bash
#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/zanna_build_lock.sh
# Purpose: Shared per-build-directory lock for the Unix build and install
#          drivers. Sourced, never executed.
#
# Key invariants:
#   - The lock is a symlink at $BUILD_LOCK_PATH whose target is the owner PID.
#   - A process that already owns the lock (same PID, e.g. after `exec` from
#     build_zanna_unix.sh into install_zanna_unix.sh) adopts it instead of
#     failing, so the build -> install hand-off has no unlocked window.
#   - Stale locks (owner PID no longer alive) are reclaimed.
#
# Ownership/Lifetime:
#   - acquire_build_lock installs an EXIT trap that removes the lock only when
#     this process still owns it.
#
# Links: scripts/build_zanna_unix.sh, scripts/install_zanna_unix.sh
#
#===----------------------------------------------------------------------===#

# Callers must set BUILD_LOCK_PATH before calling acquire_build_lock.

release_build_lock() {
    local exit_status=$?
    local owner_pid=""

    if [[ -L "$BUILD_LOCK_PATH" ]]; then
        owner_pid="$(readlink "$BUILD_LOCK_PATH" 2>/dev/null || true)"
    fi
    if [[ "$owner_pid" == "$$" ]]; then
        rm -f "$BUILD_LOCK_PATH"
    fi

    trap - EXIT
    exit "$exit_status"
}

install_build_lock_traps() {
    trap release_build_lock EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
}

acquire_build_lock() {
    local attempt owner_pid current_owner

    for attempt in 1 2 3; do
        if ln -s "$$" "$BUILD_LOCK_PATH" 2>/dev/null; then
            install_build_lock_traps
            return 0
        fi

        if [[ ! -L "$BUILD_LOCK_PATH" ]]; then
            echo "error: build lock path exists but is not a symbolic link: $BUILD_LOCK_PATH" >&2
            echo "Remove it if no Zanna build is using this build directory." >&2
            return 1
        fi

        owner_pid="$(readlink "$BUILD_LOCK_PATH" 2>/dev/null || true)"
        if [[ "$owner_pid" == "$$" ]]; then
            # Inherited across exec: traps do not survive exec, so re-arm them.
            install_build_lock_traps
            return 0
        fi
        if [[ "$owner_pid" =~ ^[0-9]+$ ]] && kill -0 "$owner_pid" 2>/dev/null; then
            echo "error: another Zanna build is already using ${BUILD_LOCK_PATH%/*} (PID $owner_pid)" >&2
            echo "Wait for it to finish or set ZANNA_BUILD_DIR to a different directory." >&2
            return 1
        fi

        # Only unlink the stale lock if it still names the owner inspected above;
        # another contender may have reclaimed it between readlink and this point.
        current_owner="$(readlink "$BUILD_LOCK_PATH" 2>/dev/null || true)"
        if [[ "$current_owner" == "$owner_pid" ]]; then
            rm -f "$BUILD_LOCK_PATH"
        fi
    done

    echo "error: unable to acquire build lock for ${BUILD_LOCK_PATH%/*}" >&2
    return 1
}
