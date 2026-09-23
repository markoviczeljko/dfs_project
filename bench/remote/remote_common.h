#ifndef DFS_BENCH_REMOTE_REMOTE_COMMON_H_
#define DFS_BENCH_REMOTE_REMOTE_COMMON_H_

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// bench/remote/*: same measurement code and dfs_bench:: helpers as
// bench/*.cpp's loopback experiments, but pointed at an already-running
// cluster (real multipass VMs, real network) instead of spawning one
// locally via process_cluster.h's spawn_cluster() — see
// docs/benchmark_results_vm.md's methodology header for why this exists
// (mentor topology review point) and docs/design.md for the shared
// experiment definitions this mirrors.
namespace dfs_bench_remote {

// One already-running node's address, parsed from a --master/--storage CLI
// flag. Non-destructive experiments (sequential/random read, cache on/off,
// Experiment 4a's write phase) only ever need host+port — real network
// GET_STATS/READ/WRITE calls, no VM involvement beyond the address.
// Destructive experiments (4b's recovery phase, node failure) additionally
// need vm_name so kill_remote_storage()/restart_remote_storage() below know
// which multipass instance to act on; vm_name is passed straight through to
// vm_script, this binary never talks to multipass/SSH itself.
struct RemoteNode {
    std::string host;
    uint16_t port = 0;
    std::string vm_name;  // empty if this node is never killed/restarted
};

struct RemoteCluster {
    RemoteNode master;
    std::vector<RemoteNode> storage;
    // Path to the VM orchestration script (scripts/vm_cluster.sh in this
    // run — kept outside the repo like all prior VM tooling, see
    // session notes). Only required by experiments that call
    // kill_remote_storage()/restart_remote_storage().
    std::string vm_script;
    // Optional --rf value, purely a label for the output table:
    // replication_factor is fixed for a live Master's whole lifetime (see
    // bench/bench_replication_factor.cpp's header comment), so unlike the
    // loopback version this binary never sets it — it just reports which
    // already-provisioned cluster (RF=1 or RF=3) it was pointed at. 0 means
    // not passed / not applicable.
    uint32_t replication_factor_label = 0;
};

// Parses "--master host:port --storage host:port[:vm_name]
// [--storage ...] [--vm-script path] [--rf N]" from argv. Terminates via
// dfs_bench::fail() (matching every other bench/*.cpp error path) on any
// malformed or missing required argument — a benchmark run with a bad
// topology argument has no partial result worth reporting.
RemoteCluster parse_remote_args(int argc, char** argv);

// Shells out to `<vm_script> kill_storage <vm_name>` — a short, synchronous
// `multipass exec ... pkill`, NOT the long-running-server-start path that
// was the documented source of multipass exec hangs in the manual VM
// verification session (see docs/design.md's remote benchmark notes) — and
// blocks until it returns. Calls dfs_bench::fail() if the targeted node has
// no vm_name (i.e. wasn't meant to be killed) or the script exits non-zero.
void kill_remote_storage(const RemoteCluster& cluster, size_t storage_index);

// Shells out to `<vm_script> restart_storage <vm_name>` (wipes that node's
// data dir and starts a fresh dfs_storage on it — the one long-running-
// process-start path this binary triggers, so vm_cluster.sh's own
// implementation is what applies the auto-background-then-verify
// workaround), then polls the node's port directly over the network via
// dfs_test::wait_for_port_ready() before returning, so callers never race a
// not-yet-listening process. Calls dfs_bench::fail() on script failure or a
// readiness timeout.
void restart_remote_storage(const RemoteCluster& cluster, size_t storage_index);

// Polls the SUM of every storage node's `used_bytes` until it stops
// changing for a short stable window, or fails via dfs_bench::fail() if it
// never settles within `timeout`. Required after restart_remote_storage():
// a node that rejoins with an empty (wiped) disk still has metadata-store
// entries pointing at it for every block it held before this whole
// cluster-lifetime's history (Phase A's writes, prior repeats' recovery
// test files, ...) — Master correctly notices those blocks are now
// under-replicated and re-replicates them elsewhere, exactly as designed.
// That legitimate churn must be allowed to finish before the next
// repeat's single-write used_bytes delta is measured, or it gets
// misattributed to that repeat's write (found while sanity-testing this
// binary locally — see the loopback experiments' comment on why THEY
// avoid this entirely by respawning a brand-new cluster per repeat
// instead of reusing one across a whole VM session).
void wait_for_storage_quiescent(const RemoteCluster& cluster,
                                 std::chrono::milliseconds timeout = std::chrono::seconds(30));

}  // namespace dfs_bench_remote

#endif  // DFS_BENCH_REMOTE_REMOTE_COMMON_H_
