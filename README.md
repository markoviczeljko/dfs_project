# dfs_project

Pojednostavljeni GFS-nalik distribuirani fajl sistem u C++20, razvijen
kao diplomski rad.

Arhitektura: Master (kontrolna ravan) + Storage Node-ovi (podatkovna
ravan) + Client. Detalji u `docs/architecture.md`, `docs/requirements.md`,
`docs/network_protocol.md`.

## Zavisnosti i okruženje

- Linux (razvijano i testirano na Ubuntu 24.04)
- CMake >= 3.20 (korišćen 3.28)
- C++20 kompajler — GCC >= 13 (korišćen GCC 13.3)
- pthreads (dio standardne sistemske biblioteke)
- GoogleTest v1.15.2 — koristi se samo za testove. CMake ga automatski
  preuzima sa GitHub-a pri prvom konfigurisanju (`tests/CMakeLists.txt`,
  `FetchContent`), pa je za prvi build potreban internet.

Druge spoljne biblioteke se ne koriste. Na Ubuntu-u je dovoljno:

```bash
sudo apt install cmake g++
```

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Izvršni fajlovi nastaju u `build/`: `dfs_master`, `dfs_storage`,
`dfs_client`, `dfs_statsctl`; benchmark alati u `build/bench/`.

### Build bez interneta

GoogleTest se može preuzeti unaprijed
(<https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip>),
raspakovati, i CMake-u proslijediti lokalna putanja umjesto preuzimanja:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/putanja/do/googletest-1.15.2 \
      -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build
```

## Pokretanje

- `scripts/test_cluster.sh` — podiže lokalni klaster (Master + 3 Storage
  čvora na portovima 9000 i 9101–9103, konfiguracije u `configs/`),
  izvršava CREATE/WRITE/READ/APPEND/STAT/LIST/DELETE preko klijenta i
  gasi klaster.
- `scripts/start_master.sh`, `scripts/start_storage.sh N` — ručno
  pokretanje Master-a i N-tog Storage čvora (`configs/storageN.conf`).
- Klijent: `./build/dfs_client --master-host 127.0.0.1 --master-port 9000
  <create|write|read|append|delete|list|stat> ...` (bez argumenata
  ispisuje uputstvo).
- `scripts/run_benchmarks.sh` — pokreće svih 5 eksperimenata i upisuje
  rezultate u `docs/benchmark_results.md`.

## Struktura

- `src/`, `include/` — Master, Storage Node, Client, mrežni sloj, statistika
- `tests/` — jedinični, integracioni, konkurentni i testovi otkaza
  (pravi procesi)
- `bench/` — eksperimenti iz rada; `bench/remote/` — ista mjerenja nad
  klasterom virtuelnih mašina (rezultati u `docs/benchmark_results_vm.md`)
- `configs/` — primjeri konfiguracija za Master i Storage čvorove
- `docs/` — arhitektura, protokol, zahtjevi, rezultati mjerenja

## Napomena o testovima

Integracioni testovi koriste fiksne lokalne TCP portove. Ako
neki drugi program slučajno koristi isti port, test koji ga koristi može
pasti pri pokretanju klastera (`start()` vraća grešku) — ponovno
pokretanje nakon što se port oslobodi rješava problem.
