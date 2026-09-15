#!/usr/bin/env bash
# Starts the dfs_master instance using configs/master.conf. Must be started
# before any start_storage.sh — Storage Nodes register with Master
# synchronously at startup and fail fast if it isn't reachable yet (Faza 9,
# see docs/design.md §13).
set -euo pipefail

cd "$(dirname "$0")/.."

exec ./build/dfs_master --config configs/master.conf
