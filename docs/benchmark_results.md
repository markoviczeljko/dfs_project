# DFS Benchmark Results

Generisano: 2026-09-17 01:43:53 UTC (`scripts/run_benchmarks.sh`).

## Metodološke napomene

- **Network/disk traffic je aproksimacija, ne pravi packet/syscall
  brojač** — vidi `docs/design.md` §22 i §24 za tačnu formulu po
  eksperimentu (READ: `cache_misses * block_size`; WRITE: računato
  analitički kao `logicki_bajtovi * replication_factor`, egzaktno
  jer klijent šalje pun chunk svakoj replici).
- **Broj konkurentnih klijenata je smanjen na {1, 5, 10}** (§16.8 iz
  dizajn dokumenta traži 1/5/10/25/50/100) — vidi `docs/design.md`
  §23 za rezonovanje. Za finalne brojeve za odbranu diplomskog rada,
  proširiti `kClientCounts` nizove u `bench/*.cpp` i ponovo
  pokrenuti na ciljnom hardveru.
- **"Recovery start time" (§10.37) nije zasebno mjeren** —
  `RecoveryManager::handle_node_failure()` postavlja recovery posao
  u red SINHRONO unutar istog callback-a koji uvećava
  `detected_failures`, pa ne postoji mjerljiv razmak između
  "detektovano" i "recovery počeo" — vidi `docs/design.md` §25.
- Brojevi ispod su iz JEDNOG razvojnog sandbox okruženja, ne
  produkcionog hardvera — pokazuju TRENDOVE (cache ubrzava READ,
  viši RF poskupljuje WRITE/storage, RF=1 nema fault tolerance), što
  je ono što diplomski rad argumentuje, ne apsolutne brojeve.

### Eksperiment 1 — Sequential READ

| Veličina fajla | Broj klijenata | Throughput (MB/s) | Prosječna latencija | P95 latencija |
| --- | --- | --- | --- | --- |
| 1 KB | 1 | 7.11 | 0.104 ms | 0.262 ms |
| 1 KB | 5 | 31.16 | 0.109 ms | 0.238 ms |
| 1 KB | 10 | 65.49 | 0.110 ms | 0.155 ms |
| 64 KB | 1 | 263.99 | 0.212 ms | 0.305 ms |
| 64 KB | 5 | 1427.67 | 0.177 ms | 0.223 ms |
| 64 KB | 10 | 2409.49 | 0.207 ms | 0.569 ms |
| 1 MB | 1 | 1449.40 | 0.672 ms | 0.270 ms |
| 1 MB | 5 | 4527.96 | 1.012 ms | 0.919 ms |
| 1 MB | 10 | 5528.15 | 1.514 ms | 1.365 ms |
| 10 MB | 1 | 1750.15 | 5.694 ms | 2.145 ms |
| 10 MB | 5 | 3828.20 | 12.357 ms | 19.503 ms |
| 10 MB | 10 | 5113.97 | 15.996 ms | 22.031 ms |
| 64 MB | 1 | 887.29 | 69.773 ms | 52.449 ms |
| 64 MB | 5 | 3563.59 | 85.161 ms | 77.494 ms |
| 64 MB | 10 | 4878.27 | 116.307 ms | 155.226 ms |

### Eksperiment 2 — Random READ

| Broj klijenata | Throughput (MB/s) | Prosječna latencija | P95 latencija | Cache hit ratio |
| --- | --- | --- | --- | --- |
| 1 | 92.39 | 10.788 ms | 15.531 ms | 13.3% |
| 5 | 560.75 | 8.184 ms | 16.230 ms | 14.7% |
| 10 | 957.05 | 9.044 ms | 17.450 ms | 15.7% |

### Eksperiment 3 — Cache ON vs OFF

| Cache | Broj klijenata | Throughput (MB/s) | Prosječna latencija | P95 latencija | Cache hit ratio | Aproks. network/disk |
| --- | --- | --- | --- | --- | --- | --- |
| OFF | 1 | 146.41 | 68.282 ms | 72.395 ms | 0.0% | 200.00 MB |
| OFF | 5 | 682.74 | 71.898 ms | 82.790 ms | 0.0% | 1000.00 MB |
| OFF | 10 | 950.80 | 97.863 ms | 133.241 ms | 0.0% | 2000.00 MB |
| ON | 1 | 970.05 | 10.287 ms | 83.315 ms | 90.0% | 20.00 MB |
| ON | 5 | 3503.59 | 14.035 ms | 80.618 ms | 90.0% | 100.00 MB |
| ON | 10 | 5214.41 | 16.172 ms | 117.387 ms | 90.0% | 200.00 MB |

### Eksperiment 4a — Replication Factor: write latencija i storage potrošnja

| RF | Prosječna write latencija | P95 latencija | Ukupno (logički) | Network estimate (tačno) | Storage izmjereno |
| --- | --- | --- | --- | --- | --- |
| 1 | 18.494 ms | 20.502 ms | 20.00 MB | 20.00 MB | 20.00 MB |
| 3 | 47.445 ms | 55.618 ms | 20.00 MB | 60.00 MB | 60.00 MB |

### Eksperiment 4b — Replication Factor: recovery ponašanje

| RF | Ishod | Vrijeme do ishoda | detected_failures (Master) | successful_recoveries (Master) |
| --- | --- | --- | --- | --- |
| 1 | NIJE OPORAVLJENO (nema izvora) | 40.02 s | 1 | 0 |
| 3 | OPORAVLJENO | 12.01 s | 1 | 1 |

### Eksperiment 5 — Storage Node Failure

| Veličina fajla | Failure detection vrijeme | Recovery trajanje | Bytes recovered | Finalno replication stanje |
| --- | --- | --- | --- | --- |
| 1.00 MB | 12.09 s | 0.00 s | 1.00 MB | 3/3 |
| 10.00 MB | 12.09 s | 0.00 s | 10.00 MB | 3/3 |

