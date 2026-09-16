#ifndef DFS_TESTS_FAILURE_PROCESS_CLUSTER_H_
#define DFS_TESTS_FAILURE_PROCESS_CLUSTER_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

#include "common/config.h"
#include "common/errors.h"
#include "common/types.h"

// Faza 16: real-OS-process test harness (see docs/design.md §20). Unlike
// every prior integration test, which constructs Master/StorageNode as
// in-process C++ objects, these helpers spawn the actual dfs_master/
// dfs_storage/dfs_client/dfs_statsctl binaries as separate processes, so
// tests here exercise real TCP across process boundaries and real signal-
// based process termination (SIGKILL) — neither observable from inside a
// single process.
namespace dfs_test {

// RAII handle for one spawned process. The destructor is a hard safety
// net: if the process is still running when the handle is destroyed (e.g.
// a test aborts mid-ASSERT before its own explicit cleanup runs), it sends
// SIGKILL and reaps it — a test must never leak a live dfs_master/
// dfs_storage process into the rest of the test run.
class ProcessHandle {
public:
    ProcessHandle() = default;
    ~ProcessHandle();

    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&& other) noexcept;
    ProcessHandle& operator=(ProcessHandle&& other) noexcept;

    // executable_path is argv[0]; args are the remaining argv entries.
    static Result<ProcessHandle> spawn(const std::string& executable_path,
                                        const std::vector<std::string>& args);

    ErrorCode send_signal(int sig) const;

    // Blocks (polling) up to `timeout` for the process to exit. Returns
    // the raw wait() status if it exited, std::nullopt if `timeout`
    // elapsed while it was still running (the process is NOT killed in
    // that case — the caller decides what to do).
    std::optional<int> wait_for_exit(std::chrono::milliseconds timeout);

    bool is_running() const;
    pid_t pid() const { return pid_; }

private:
    pid_t pid_ = -1;
    bool reaped_ = true;
};

// Polls connect() to (host, port) until it succeeds or `timeout` elapses.
// Neither dfs_master nor dfs_storage exposes any other readiness signal
// (no pidfile, no health endpoint) at the moment a process is spawned.
bool wait_for_port_ready(const std::string& host, uint16_t port, std::chrono::milliseconds timeout);

// Writes a master.conf-format file (see src/master/main_master.cpp's
// parser) under `dir` and returns its path. replication_factor (Faza 17.1)
// defaults to DEFAULT_REPLICATION_FACTOR — always written explicitly so a
// test config is self-documenting, even when it matches the default.
std::filesystem::path write_master_config(
    const std::filesystem::path& dir, uint16_t port, const std::filesystem::path& metadata_dir,
    uint32_t replication_factor = DEFAULT_REPLICATION_FACTOR);

// Writes a storageN.conf-format file (see src/storage/main_storage.cpp's
// parser) under `dir` and returns its path.
std::filesystem::path write_storage_config(const std::filesystem::path& dir, StorageNodeId node_id,
                                            uint16_t port, const std::filesystem::path& data_dir,
                                            uint16_t master_port,
                                            uint64_t capacity_bytes = 1024ULL * 1024 * 1024);

// Spawns a fresh dfs_storage process against an already-written config
// file (e.g. to restart a node after a graceful stop, reusing the same
// node_id/port/data_dir it already had) — the CMake-generated binary path
// stays an implementation detail of process_cluster.cpp, never needed
// directly by a test file.
Result<ProcessHandle> respawn_storage_node(const std::filesystem::path& config_path);

// True if `data_dir/blocks/` contains at least one `*.block` file. Avoids
// needing to parse `dfs_client stat`'s text output for a numeric BlockId
// just to check which real Storage Node process physically holds a given
// (single-block) file's data — the test controls every data_dir path
// itself, so a direct filesystem check is simpler and just as reliable.
bool has_any_block_on_disk(const std::filesystem::path& data_dir);

// Runs dfs_client once (a one-shot process, see src/client/main_client.cpp)
// against the Master listening on `master_port`, blocking until it exits.
// stdout/stderr are inherited (visible under `ctest -V`), not captured —
// callers only need the exit code. Returns -1 on spawn failure or if the
// process doesn't exit within a generous internal timeout.
int run_client(uint16_t master_port, const std::vector<std::string>& args);

// Runs `dfs_statsctl --host 127.0.0.1 --port <target_port> stats` and
// returns its captured stdout (the raw STATS_RESPONSE text) — the one
// helper that needs to capture output rather than just an exit code,
// since callers need to inspect specific counters in it.
std::string run_statsctl(uint16_t target_port);

// A real Master + N real Storage Node processes, all rooted under `dir`.
// Bundles the repeated spawn-then-wait-for-ready sequence every failure
// test needs. stop_all() sends SIGTERM to everything and waits briefly —
// best-effort graceful shutdown; ProcessHandle's own destructor is still
// the hard safety net if a process doesn't exit in time.
struct SpawnedCluster {
    ProcessHandle master;
    std::vector<ProcessHandle> storage_nodes;
    std::vector<std::filesystem::path> storage_data_dirs;
    std::vector<std::filesystem::path> storage_configs;

    void stop_all();
};

// Spawns a real Master and one real Storage Node per entry in
// `storage_ports`, waits for every port to accept connections (bounded by
// `ready_timeout`, applied independently to each process — not a shared
// budget). Returns std::nullopt if any process fails to become ready.
// replication_factor (Faza 17.1) defaults to DEFAULT_REPLICATION_FACTOR —
// pass a lower value (with enough storage_ports to still satisfy it) to
// spawn a cluster for Faza 17.4's RF=1-vs-RF=3 benchmark/test scenarios.
std::optional<SpawnedCluster> spawn_cluster(
    const std::filesystem::path& dir, uint16_t master_port,
    const std::vector<uint16_t>& storage_ports, std::chrono::milliseconds ready_timeout,
    uint32_t replication_factor = DEFAULT_REPLICATION_FACTOR);

}  // namespace dfs_test

#endif  // DFS_TESTS_FAILURE_PROCESS_CLUSTER_H_
