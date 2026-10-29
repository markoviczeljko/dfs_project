# DFS — A GFS-Inspired Distributed File System in C++20

A distributed file system built from scratch in modern C++, modeled on the
ideas behind the Google File System (GFS). Files are split into blocks,
replicated across storage nodes, and automatically re-replicated when a node
fails. A single Master manages metadata, and file data flows directly between
clients and storage nodes.

**Highlights**

- ~17.5k lines of C++20, with no external runtime dependencies (only the
  standard library and POSIX sockets/pthreads)
- Custom binary protocol over TCP
- 3-way block replication, with replica writes fanned out in parallel
- Heartbeat-based failure detection and automatic re-replication
- Crash recovery for both the Master and the Storage Nodes
- Client-side block-level LRU cache
- 274 GoogleTest test cases: unit, integration, concurrency, and
  failure-injection tests that run real multi-process clusters
- Benchmark suite run both on loopback and on a 5-VM cluster

---

## Architecture

```
                    ┌──────────────────────────┐
                    │          Master          │   control plane
                    │  namespace · metadata    │
                    │  block placement         │
                    │  failure detection       │
                    │  recovery coordination   │
                    └────────────┬─────────────┘
          metadata ops           │  heartbeats / registration
   (create, stat, locate…)       │
┌──────────┐ ◄───────────────────┤
│  Client  │                     │
│ LRU cache│                     │
└────┬─────┘                     │
     │  block data (read/write)  │
     ▼                           ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ Storage Node │  │ Storage Node │  │ Storage Node │   data plane
│   blocks +   │  │   blocks +   │  │   blocks +   │
│   checksums  │  │   checksums  │  │   checksums  │
└──────────────┘  └──────────────┘  └──────────────┘
```

The design rests on a strict split between the control plane and the data
plane: **the Master never carries file data**.

| Component | Responsibility |
|-----------|----------------|
| **Master** (`dfs_master`) | Hierarchical namespace, file → block mapping, replica placement, Storage Node registry, heartbeat-based failure detection, re-replication of lost blocks, persistent metadata snapshot |
| **Storage Node** (`dfs_storage`) | Stores blocks on local disk with checksums and versions, serves reads and writes from clients, copies blocks between nodes during recovery, keeps an idempotency log so retried writes are safe |
| **Client** (`dfs_client`) | Exposes `create / write / read / append / delete / list / stat`, asks the Master for metadata, transfers data directly with Storage Nodes, caches blocks in a local LRU cache |
| **Stats tool** (`dfs_statsctl`) | Queries runtime statistics (counters, latency histograms) from any running node |

### Key design points

- **Block-based storage:** files are split into 64 MB blocks, and each block
  is stored on 3 nodes by default. The replication factor can be set at
  runtime.
- **Write path:** the client gets a block allocation from the Master, sends
  the data to all replicas in parallel, then commits. Blocks that were
  allocated but never committed are cleaned up with an explicit
  `ABANDON_BLOCKS` step.
- **Failure handling:** Storage Nodes send a heartbeat every 2 s. A node that
  stays silent for 6 s is declared dead, and its blocks are re-replicated from
  the surviving copies onto healthy nodes.
- **Durability and restart:** the Master persists its metadata as a snapshot
  protected by a CRC check. On restart, a Storage Node verifies the checksum of
  every block and reconciles its block inventory with the Master.
- **Idempotency:** mutating operations carry an operation ID. A request
  retried after a lost ACK replays the cached response instead of running the
  operation a second time.
- **Concurrency:** the server uses one thread per connection, and concurrent
  reads, writes, and appends are covered by a dedicated test suite.

---

## Benchmark highlights

Each experiment was run 3 times per configuration, both on loopback (all
processes on one machine) and on a **5-VM cluster** (multipass/KVM: 1 Master
VM and 4 Storage VMs, each with its own IP and virtual disk). Hardware:
Intel i7-13620H, 32 GB RAM, NVMe SSD.

| Experiment | Result (5-VM cluster) |
|------------|-----------------------|
| Client cache ON vs OFF (90% hit ratio, 1 client) | **~6.9× higher throughput** (107 → 735 MB/s), ~10× less data read from Storage Nodes |
| Sequential read, 10 MB file | ~950 MB/s with 1 client |
| Storage Node failure (RF=3) | Failure detected in ~12 s, every block re-replicated (3/3 runs) |
| Replication factor 1 vs 3 | RF=1 never recovers after a node loss (0/3 runs). RF=3 always recovers (3/3) at 3× the storage cost |

On loopback, the parallel replica fan-out brought RF=3 write latency down to
1.07× that of RF=1 (12.3 ms vs 11.5 ms for a 20 MB write). With sequential
replica writes it was 2.4×.

Full results and methodology:
[`docs/benchmark_results.md`](docs/benchmark_results.md) (loopback) and
[`docs/benchmark_results_vm.md`](docs/benchmark_results_vm.md) (VM cluster).

---

## Getting started

### Requirements

- Linux (developed and tested on Ubuntu 24.04)
- CMake ≥ 3.20
- A C++20 compiler (GCC ≥ 13)
- GoogleTest v1.15.2, used for tests only. CMake fetches it automatically on
  the first configure.

```bash
sudo apt install cmake g++
```

### Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

The build produces `dfs_master`, `dfs_storage`, `dfs_client`, and
`dfs_statsctl` in `build/`. The benchmark binaries go in `build/bench/`.

<details>
<summary>Offline build (without downloading GoogleTest)</summary>

Download
<https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip>, unpack
it, and point CMake at the local copy:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest-1.15.2 \
      -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build -j
```
</details>

### Run a local cluster

The quickest way to see the system work is the demo script. It starts a
Master and 3 Storage Nodes, runs every client operation, and shuts the
cluster down:

```bash
./scripts/test_cluster.sh
```

To start the cluster by hand (configs are in `configs/`):

```bash
./scripts/start_master.sh            # Master on port 9000
./scripts/start_storage.sh 1         # Storage Nodes on ports 9101–9103
./scripts/start_storage.sh 2
./scripts/start_storage.sh 3
```

Then use the client:

```bash
DFS="./build/dfs_client --master-host 127.0.0.1 --master-port 9000"

$DFS create /docs/report.txt
$DFS write  /docs/report.txt ./local_file.txt
$DFS append /docs/report.txt ./more_data.txt
$DFS read   /docs/report.txt ./downloaded.txt
$DFS stat   /docs/report.txt
$DFS list   /docs
$DFS delete /docs/report.txt
```

To inspect the runtime statistics of a node:

```bash
./build/dfs_statsctl --host 127.0.0.1 --port 9000 stats
```

### Run the benchmarks

```bash
./scripts/run_benchmarks.sh   # runs all 5 experiments, writes docs/benchmark_results.md
```

---

## Project structure

```
include/, src/
  master/       namespace, metadata store, storage registry,
                failure detector, replication and recovery managers
  storage/      block storage (checksummed), block manager,
                operation log (idempotency), local recovery
  client/       client API and LRU block cache
  network/      socket layer, thread-per-connection server, binary protocol
  statistics/   counters, latency histograms, dfs_statsctl
  common/       shared types, config parsing, error codes, logger
tests/
  unit/         per-component tests
  integration/  end-to-end operations against an in-process cluster
  concurrency/  concurrent reads, writes and appends
  failure/      real multi-process clusters with injected node failures
bench/          the 5 benchmark experiments (loopback)
bench/remote/   the same experiments against a remote (VM) cluster
configs/        sample Master and Storage Node configs
scripts/        cluster start/demo and benchmark orchestration
docs/           architecture, design decisions, benchmark results
```

---

## Notes

- The integration tests use fixed local TCP ports. If another program holds
  one of those ports, the affected test can fail when the cluster starts.
  Rerun the test once the port is free.
- Known limitation: the client cache does not keep caches consistent across
  clients. A client invalidates only its own cached blocks after its own
  writes. Revalidating cache hits has been designed but not implemented
  (see `docs/design_faza19_cache_revalidation.md`).

## Author

**Željko Marković** — [markovic.zeljko2002@gmail.com](mailto:markovic.zeljko2002@gmail.com)
