#!/usr/bin/env bash
# Starts the dfs_master instance using configs/master.conf.
set -euo pipefail

cd "$(dirname "$0")/.."

exec ./build/dfs_master --config configs/master.conf
