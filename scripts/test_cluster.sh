#!/usr/bin/env bash
# Faza 16: operational demo script for the diploma thesis defense — brings
# up a real 4-process cluster (Master + 3 Storage Nodes) on the fixed
# configs/ ports, runs a client-driven CREATE/WRITE/READ/APPEND/STAT/LIST/
# DELETE smoke sequence against it, and tears everything down. Manual-only
# by design (see docs/design.md §20): fixed ports 9000/9101-9103 aren't
# safe for repeated/parallel automated runs, so this is NOT wired into
# CTest — the automated regression coverage for real multi-process
# behavior lives in tests/failure/*.cpp instead, which use dynamic ports.
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p logs
pids=()
tmp_src=""
tmp_dst=""

cleanup() {
    rm -f "$tmp_src" "$tmp_dst"
    for pid in "${pids[@]:-}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    for _ in $(seq 1 20); do
        alive=0
        for pid in "${pids[@]:-}"; do
            kill -0 "$pid" 2>/dev/null && alive=1
        done
        [ "$alive" -eq 0 ] && break
        sleep 0.5
    done
    for pid in "${pids[@]:-}"; do
        kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
    done
}
# Separate signal traps from the EXIT trap and exit explicitly — bash
# overrides the default terminate-on-signal behavior once a trap is
# installed for INT/TERM, so without an explicit `exit` here, cleanup()
# would run and then execution would resume at the interrupted line
# instead of actually stopping (e.g. a demo interrupted mid-startup would
# fall through into the smoke sequence against an already-dead cluster).
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap cleanup ERR

wait_for_port() { # host port timeout_seconds
    local host="$1" port="$2" timeout="$3" waited=0
    while ! (exec 3<>"/dev/tcp/${host}/${port}") 2>/dev/null; do
        sleep 0.2
        waited=$((waited + 1))
        if [ "$waited" -ge $((timeout * 5)) ]; then
            echo "test_cluster.sh: timed out waiting for ${host}:${port}" >&2
            exit 1
        fi
    done
}

# Master must start before Storage Nodes — they register with it
# synchronously at startup and fail fast if it isn't reachable yet.
./build/dfs_master --config configs/master.conf >>logs/master.log 2>&1 &
pids+=("$!")
wait_for_port 127.0.0.1 9000 15

for n in 1 2 3; do
    mkdir -p "data/storage${n}"
    ./build/dfs_storage --config "configs/storage${n}.conf" >>"logs/storage${n}.log" 2>&1 &
    pids+=("$!")
done
wait_for_port 127.0.0.1 9101 15
wait_for_port 127.0.0.1 9102 15
wait_for_port 127.0.0.1 9103 15

client() { ./build/dfs_client --master-host 127.0.0.1 --master-port 9000 "$@"; }

tmp_src="$(mktemp)"
tmp_dst="$(mktemp)"
printf 'hello dfs' >"$tmp_src"

echo "test_cluster.sh: running smoke sequence"
client create /smoke.txt
client write /smoke.txt "$tmp_src"
client read /smoke.txt "$tmp_dst"
cmp "$tmp_src" "$tmp_dst"
client append /smoke.txt "$tmp_src"
client stat /smoke.txt
client list /
client delete /smoke.txt
echo "test_cluster.sh: smoke sequence OK"
