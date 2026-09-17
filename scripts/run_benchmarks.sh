#!/usr/bin/env bash
# Faza 17.5: runs all 5 bench/ executables (Faza 17.3-17.5) in sequence
# and writes their combined Markdown output to docs/benchmark_results.md.
# Manual-only, like scripts/test_cluster.sh — not wired into CTest (these
# are measurement tools, not correctness tests, see IMPLEMENTATION_PLAN.md's
# Faza 17 section).
#
# Unlike test_cluster.sh, this script does NOT need its own trap-based
# process cleanup: each bench/ executable spawns and fully owns its own
# real Master + Storage Node cluster, and already tears it down on both
# normal completion and any internal failure
# (bench/bench_common.cpp's register_cluster_for_cleanup()/fail(), see
# docs/design.md §23). A Ctrl-C during this script doesn't leak orphaned
# dfs_master/dfs_storage processes either: they inherit this script's
# process group (posix_spawn's default, see
# tests/failure/process_cluster.cpp) and already install their own
# graceful SIGINT/SIGTERM handler (src/master/main_master.cpp,
# src/storage/main_storage.cpp) — SIGINT reaches them directly and they
# shut themselves down, independent of whether the interrupted bench
# executable's own stop_all() ever runs.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build}"
OUTPUT_FILE="docs/benchmark_results.md"

BENCHMARKS=(
    bench_sequential_read
    bench_random_read
    bench_cache_on_off
    bench_replication_factor
    bench_node_failure
)

for name in "${BENCHMARKS[@]}"; do
    if [ ! -x "$BUILD_DIR/bench/$name" ]; then
        echo "error: $BUILD_DIR/bench/$name not found or not executable" >&2
        echo "       build it first: cmake --build $BUILD_DIR" >&2
        exit 1
    fi
done

{
    echo "# DFS Benchmark Results"
    echo
    echo "Generisano: $(date -u +"%Y-%m-%d %H:%M:%S UTC") (\`scripts/run_benchmarks.sh\`)."
    echo
    echo "## Metodološke napomene"
    echo
    echo "- **Network/disk traffic je aproksimacija, ne pravi packet/syscall"
    echo "  brojač** — vidi \`docs/design.md\` §22 i §24 za tačnu formulu po"
    echo "  eksperimentu (READ: \`cache_misses * block_size\`; WRITE: računato"
    echo "  analitički kao \`logicki_bajtovi * replication_factor\`, egzaktno"
    echo "  jer klijent šalje pun chunk svakoj replici)."
    echo "- **Broj konkurentnih klijenata je smanjen na {1, 5, 10}** (§16.8 iz"
    echo "  dizajn dokumenta traži 1/5/10/25/50/100) — vidi \`docs/design.md\`"
    echo "  §23 za rezonovanje. Za finalne brojeve za odbranu diplomskog rada,"
    echo "  proširiti \`kClientCounts\` nizove u \`bench/*.cpp\` i ponovo"
    echo "  pokrenuti na ciljnom hardveru."
    echo "- **\"Recovery start time\" (§10.37) nije zasebno mjeren** —"
    echo "  \`RecoveryManager::handle_node_failure()\` postavlja recovery posao"
    echo "  u red SINHRONO unutar istog callback-a koji uvećava"
    echo "  \`detected_failures\`, pa ne postoji mjerljiv razmak između"
    echo "  \"detektovano\" i \"recovery počeo\" — vidi \`docs/design.md\` §25."
    echo "- Brojevi ispod su iz JEDNOG razvojnog sandbox okruženja, ne"
    echo "  produkcionog hardvera — pokazuju TRENDOVE (cache ubrzava READ,"
    echo "  viši RF poskupljuje WRITE/storage, RF=1 nema fault tolerance), što"
    echo "  je ono što diplomski rad argumentuje, ne apsolutne brojeve."
    echo
    for name in "${BENCHMARKS[@]}"; do
        echo "Pokrećem $name..." >&2
        # Each bench executable's spawned dfs_master/dfs_storage children
        # inherit its stdout fd (see tests/failure/process_cluster.cpp's
        # ProcessHandle::spawn(), reused unchanged from Faza 16) and print
        # their own "listening on port"/"shutting down" lines directly to
        # it — meanwhile the bench executable's own print_markdown_table()
        # output sits in libstdc++'s buffer (stdout isn't a TTY once
        # redirected to a file, so it's fully buffered, not line-buffered)
        # until the process exits. Net effect: all of a run's child noise
        # lands in the raw output BEFORE its own table text, even though
        # the table was logically printed first in program order. Filtered
        # out here via the first `### ` heading — every bench executable's
        # actual results always start with one — rather than modifying the
        # shared process-spawning harness just to suppress it, which would
        # also affect tests/failure/*.cpp's `ctest -V` debugging visibility.
        raw_output="$("$BUILD_DIR/bench/$name")"
        echo "$raw_output" | sed -n '/^### /,$p'
        echo
    done
} > "$OUTPUT_FILE"

echo "Rezultati zapisani u $OUTPUT_FILE" >&2
