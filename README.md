# dfs_project

Pojednostavljeni GFS-nalik distribuirani fajl sistem u C++20, razvijen
kao diplomski rad.

Arhitektura: Master (kontrolna ravan) + Storage Node-ovi (podatkovna
ravan) + Client. Detalji u `docs/architecture.md`, `docs/requirements.md`,
`docs/network_protocol.md`.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Zavisnosti: CMake 3.20+, C++20 kompajler, GoogleTest (preuzima se
automatski preko `FetchContent`).
