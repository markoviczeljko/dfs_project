#!/usr/bin/env bash
# Starts a single dfs_storage instance using configs/storageN.conf, where N
# is the first argument (default 1). Master must already be running — a
# Storage Node registers with it synchronously at startup and fails fast if
# it isn't reachable (see docs/design.md §13). RF=3 needs at least 3
# instances running: ./scripts/start_storage.sh 1 / 2 / 3.
set -euo pipefail

cd "$(dirname "$0")/.."

node_index="${1:-1}"

mkdir -p "data/storage${node_index}"

exec ./build/dfs_storage --config "configs/storage${node_index}.conf"
