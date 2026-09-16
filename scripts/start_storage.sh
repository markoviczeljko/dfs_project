#!/usr/bin/env bash
# Starts a single dfs_storage instance using configs/storageN.conf, where N
# is the first argument (default 1). Master must already be running — a
# Storage Node registers with it synchronously at startup and fails fast if
# it isn't reachable (see docs/design.md §13). RF=3 needs at least 3
# instances running: ./scripts/start_storage.sh 1 / 2 / 3.
#
# Faza 16: writes data/storageN.pid and appends to logs/storageN.log so the
# process can be found/monitored without keeping this script's own
# terminal open. Still blocks in the foreground (Ctrl+C stops it, same as
# before) — only test_cluster.sh manages multiple such processes at once.
set -euo pipefail

cd "$(dirname "$0")/.."

node_index="${1:-1}"

mkdir -p "data/storage${node_index}" logs
pidfile="data/storage${node_index}.pid"

if [ -f "$pidfile" ]; then
    old_pid="$(cat "$pidfile" 2>/dev/null || true)"
    if [ -n "$old_pid" ] && [ "$(ps -p "$old_pid" -o comm= 2>/dev/null)" = "dfs_storage" ]; then
        echo "start_storage.sh: dfs_storage (node ${node_index}) already running (pid ${old_pid}, see ${pidfile})" >&2
        exit 1
    fi
fi

./build/dfs_storage --config "configs/storage${node_index}.conf" >>"logs/storage${node_index}.log" 2>&1 &
pid=$!
echo "$pid" >"$pidfile"
echo "start_storage.sh: dfs_storage node ${node_index} running (pid ${pid}), logs in logs/storage${node_index}.log"
wait "$pid"
