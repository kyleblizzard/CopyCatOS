#!/usr/bin/env bash
# CopyCatOS — by Kyle Blizzard at Blizzard.show

# valgrind-leak-loop.sh — 1000-launch valgrind leak check.
#
# Loops the full client lifecycle (init → window_create → cairo →
# window_commit → window_close → quit) N times under valgrind, each run
# in its own process so every allocation made by libmoonbase must be
# freed before the run exits. Any definitely-lost or indirectly-lost
# bytes fail the whole loop.
#
# We drive mb-window-commit because it exercises the widest path in the
# framework that's implemented today: the IPC handshake, WINDOW_CREATE
# round-trip with a parsed reply, Cairo allocation + shm memfd, the
# SCM_RIGHTS commit, WINDOW_CLOSE, and BYE. That covers every malloc /
# mmap path the C hello app hits in normal use.
#
# Usage:
#   bash moonbase/scripts/valgrind-leak-loop.sh          # 1000 runs
#   ITERATIONS=50 bash moonbase/scripts/valgrind-leak-loop.sh
#   TARGET=mb-hello-handshake bash moonbase/scripts/valgrind-leak-loop.sh
#
# Expected: prints "ok: N launches under valgrind, zero leaks" and exits
# 0 only if every launch was leak-clean.

set -u

ITERATIONS="${ITERATIONS:-1000}"
TARGET="${TARGET:-mb-window-commit}"
BUILD_DIR="${BUILD_DIR:-$(pwd)/build}"

BIN="${BUILD_DIR}/${TARGET}"
if [[ ! -x "$BIN" ]]; then
    echo "error: test binary not found at $BIN" >&2
    echo "run meson compile -C build first" >&2
    exit 2
fi

if ! command -v valgrind >/dev/null 2>&1; then
    echo "error: valgrind not installed" >&2
    exit 2
fi

LOG_DIR="$(mktemp -d /tmp/mb-vg.XXXXXX)"
trap 'rm -rf "$LOG_DIR"' EXIT

echo "running $ITERATIONS launches of $TARGET under valgrind..."
echo "logs: $LOG_DIR"

fail_count=0
for ((i = 1; i <= ITERATIONS; i++)); do
    log="${LOG_DIR}/run-${i}.log"
    # --error-exitcode=7 so valgrind's own verdict surfaces as the
    # process exit status separate from the test binary's normal codes.
    # --errors-for-leak-kinds=definite,indirect counts only the leak
    # classes we actually care about — "possibly lost" fires on things
    # like pthread TLS that the child doesn't own and can't free.
    if ! valgrind \
            --quiet \
            --error-exitcode=7 \
            --leak-check=full \
            --show-leak-kinds=definite,indirect \
            --errors-for-leak-kinds=definite,indirect \
            --track-origins=no \
            --child-silent-after-fork=yes \
            --log-file="$log" \
            "$BIN" >/dev/null 2>&1; then
        fail_count=$((fail_count + 1))
        echo "iteration $i FAILED — log saved at $log"
        # Keep this log even after exit so the user can read it.
        mv "$log" "${LOG_DIR%/}-fail-${i}.log" 2>/dev/null || true
        # Only show the first few failures — at that point the leak is
        # reproducible and further runs add nothing.
        if (( fail_count >= 3 )); then
            echo "three failures in a row — stopping early"
            exit 1
        fi
    fi

    # Periodic progress.
    if (( i % 50 == 0 )); then
        echo "  $i / $ITERATIONS (0 fails so far)"
    fi
done

if (( fail_count > 0 )); then
    echo "FAIL: $fail_count of $ITERATIONS launches leaked"
    exit 1
fi

echo "ok: $ITERATIONS launches under valgrind, zero leaks"
