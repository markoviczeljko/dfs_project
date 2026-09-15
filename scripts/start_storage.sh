#!/usr/bin/env bash
# Starts a single dfs_storage instance using configs/storage1.conf.
set -euo pipefail

cd "$(dirname "$0")/.."

mkdir -p data/storage1

exec ./build/dfs_storage --config configs/storage1.conf
