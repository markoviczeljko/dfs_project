#!/usr/bin/env bash
# Starts the dfs_master instance using configs/master.conf. Must be started
# before any start_storage.sh — Storage Nodes register with Master
# synchronously at startup and fail fast if it isn't reachable yet (Faza 9,
# see docs/design.md §13).
#
# Faza 16: writes data/master.pid and appends to logs/master.log so the
# process can be found/monitored without keeping this script's own
# terminal open. Still blocks in the foreground (Ctrl+C stops it, same as
# before) — only test_cluster.sh manages multiple such processes at once.
set -euo pipefail

cd "$(dirname "$0")/.."

mkdir -p data logs
pidfile="data/master.pid"

if [ -f "$pidfile" ]; then
    old_pid="$(cat "$pidfile" 2>/dev/null || true)"
    if [ -n "$old_pid" ] && [ "$(ps -p "$old_pid" -o comm= 2>/dev/null)" = "dfs_master" ]; then
        echo "start_master.sh: dfs_master already running (pid ${old_pid}, see ${pidfile})" >&2
        exit 1
    fi
fi

./build/dfs_master --config configs/master.conf >>logs/master.log 2>&1 &
pid=$!
echo "$pid" >"$pidfile"
echo "start_master.sh: dfs_master running (pid ${pid}), logs in logs/master.log"
wait "$pid"
