# DFS Benchmark Results

Generisano: 2026-09-19 08:53:26 UTC (`scripts/run_benchmarks.sh`).

## Metodološke napomene

### Hardver i okruženje

- CPU: Intel Core i7-13620H (10 jezgara / 16 niti, hibridna P+E arhitektura)
- RAM: 32 GB
- Disk: NVMe SSD, ext4 fajl sistem
- OS/kernel: Ubuntu 24.04.3 LTS, kernel 7.0.0-28-generic
- Kompajler: GCC 13, CMake 3.28.3
- Build tip: Release (`-DCMAKE_BUILD_TYPE=Release`, optimizacija uključena)
- Topologija: Master i svi Storage čvorovi su odvojeni OS procesi na
  istoj mašini, komunikacija preko loopback interfejsa (127.0.0.1).
- Broj Storage čvorova po eksperimentu: 3 za Eksperimente 1-3, 4 za
  Eksperimente 4-5 (dodatni čvor služi kao mjesto za oporavljenu repliku).

### Ponavljanja mjerenja

- Svaka ćelija (kombinacija veličine fajla/broja klijenata/moda) se
  izvršava 3 puta nezavisno; ispod se prikazuje min / mean / max preko
  ta 3 ponavljanja, ne jedno izmjereno očitavanje.
- Percentili (P95) se računaju nad objedinjenim (pooled) uzorcima iz
  sva 3 ponavljanja radi stabilnije procjene repa raspodjele.

### Ostale napomene

- **Network/disk traffic je aproksimacija, ne pravi packet/syscall
  brojač** — vidi `docs/design.md` §22 i §24 za tačnu formulu po
  eksperimentu (READ: `cache_misses * block_size`; WRITE: računato
  analitički kao `logicki_bajtovi * replication_factor`, egzaktno
  jer klijent šalje pun chunk svakoj replici).
- **Broj konkurentnih klijenata je {1, 5, 10}** (§16.8 iz dizajn
  dokumenta traži pun opseg 1/5/10/25/50/100) — proširivanje na pun
  opseg ostaje pravac budućeg rada, vidi Zaključak.
- **"Recovery start time" (§10.37) nije zasebno mjeren** —
  `RecoveryManager::handle_node_failure()` postavlja recovery posao
  u red SINHRONO unutar istog callback-a koji uvećava
  `detected_failures`, pa ne postoji mjerljiv razmak između
  "detektovano" i "recovery počeo" — vidi `docs/design.md` §25.
- Brojevi ispod pokazuju TRENDOVE (cache ubrzava READ, viši RF
  poskupljuje WRITE/storage, RF=1 nema fault tolerance), što je ono
  što diplomski rad argumentuje, ne apsolutne brojeve prenosive na
  drugačiji hardver.

### Eksperiment 1 — Sequential READ (3 ponavljanja po ćeliji, min / mean / max)

| Veličina fajla | Broj klijenata | Throughput min/mean/max (MB/s) | Latencija min/mean/max | P95 latencija (pooled) |
| --- | --- | --- | --- | --- |
| 1 KB | 1 | 7.80 / 9.28 / 10.55 | 0.078 / 0.089 / 0.097 ms | 0.190 ms |
| 1 KB | 5 | 54.20 / 67.63 / 74.56 | 0.047 / 0.050 / 0.056 ms | 0.145 ms |
| 1 KB | 10 | 102.36 / 111.07 / 116.80 | 0.061 / 0.063 / 0.065 ms | 0.155 ms |
| 64 KB | 1 | 375.07 / 486.35 / 622.28 | 0.087 / 0.112 / 0.141 ms | 0.325 ms |
| 64 KB | 5 | 2193.07 / 2543.04 / 2730.28 | 0.090 / 0.098 / 0.111 ms | 0.169 ms |
| 64 KB | 10 | 3998.41 / 4159.08 / 4476.20 | 0.116 / 0.117 / 0.118 ms | 0.415 ms |
| 1 MB | 1 | 1155.15 / 1176.92 / 1198.83 | 0.800 / 0.816 / 0.837 ms | 0.503 ms |
| 1 MB | 5 | 4610.52 / 5272.47 / 6335.65 | 0.703 / 0.848 / 0.948 ms | 0.928 ms |
| 1 MB | 10 | 7420.18 / 7550.57 / 7676.03 | 1.140 / 1.149 / 1.159 ms | 2.498 ms |
| 10 MB | 1 | 2082.70 / 2234.63 / 2426.50 | 4.097 / 4.473 / 4.786 ms | 2.081 ms |
| 10 MB | 5 | 4497.22 / 4720.86 / 4908.19 | 9.158 / 9.649 / 10.286 ms | 14.592 ms |
| 10 MB | 10 | 5840.13 / 6749.39 / 7671.15 | 11.183 / 13.142 / 14.994 ms | 32.764 ms |
| 64 MB | 1 | 969.77 / 977.98 / 983.15 | 62.397 / 62.731 / 63.256 ms | 49.776 ms |
| 64 MB | 5 | 3339.79 / 3669.22 / 3955.43 | 76.532 / 82.582 / 91.064 ms | 106.681 ms |
| 64 MB | 10 | 4806.43 / 4872.77 / 4949.73 | 117.364 / 120.108 / 122.812 ms | 193.955 ms |
| 128 MB | 1 | 959.05 / 964.89 / 970.97 | 125.810 / 126.590 / 127.265 ms | 105.196 ms |
| 128 MB | 5 | 3708.66 / 3878.19 / 4027.49 | 149.930 / 155.958 / 163.302 ms | 171.496 ms |
| 128 MB | 10 | 5481.84 / 5666.82 / 5929.19 | 200.366 / 209.695 / 215.065 ms | 284.959 ms |
| 256 MB | 1 | 942.54 / 949.57 / 954.30 | 254.073 / 255.375 / 257.526 ms | 206.156 ms |
| 256 MB | 5 | 3971.16 / 3992.15 / 4019.74 | 301.791 / 302.986 / 304.779 ms | 319.326 ms |
| 256 MB | 10 | 5064.78 / 5278.42 / 5642.51 | 417.135 / 447.435 / 463.555 ms | 580.611 ms |

### Eksperiment 2 — Random READ (3 ponavljanja po ćeliji, min / mean / max)

| Broj klijenata | Throughput min/mean/max (MB/s) | Latencija min/mean/max | P95 latencija (pooled) | Cache hit ratio min/mean/max |
| --- | --- | --- | --- | --- |
| 1 | 161.40 / 175.24 / 188.91 | 5.268 / 5.700 / 6.148 ms | 7.489 ms | 13.3% / 17.8% / 23.3% |
| 5 | 798.85 / 816.69 / 834.42 | 5.334 / 5.564 / 5.782 ms | 9.929 ms | 14.7% / 16.7% / 18.7% |
| 10 | 1617.01 / 1744.08 / 1873.10 | 5.022 / 5.224 / 5.503 ms | 10.752 ms | 15.7% / 17.8% / 20.7% |

### Eksperiment 3 — Cache ON vs OFF (3 ponavljanja po ćeliji, min / mean / max)

| Cache | Broj klijenata | Throughput min/mean/max (MB/s) | Latencija min/mean/max | P95 latencija (pooled) | Cache hit ratio min/mean/max | Aproks. network/disk (mean) |
| --- | --- | --- | --- | --- | --- | --- |
| OFF | 1 | 185.28 / 190.15 / 198.94 | 50.247 / 52.626 / 53.959 ms | 65.111 ms | 0.0% / 0.0% / 0.0% | 200.00 MB |
| OFF | 5 | 887.74 / 892.01 / 897.10 | 54.972 / 55.348 / 55.773 ms | 61.657 ms | 0.0% / 0.0% / 0.0% | 1000.00 MB |
| OFF | 10 | 1467.47 / 1495.67 / 1513.00 | 64.348 / 65.079 / 65.963 ms | 75.898 ms | 0.0% / 0.0% / 0.0% | 2000.00 MB |
| ON | 1 | 1361.24 / 1474.55 / 1592.94 | 6.256 / 6.789 / 7.327 ms | 47.466 ms | 90.0% / 90.0% / 90.0% | 20.00 MB |
| ON | 5 | 3495.65 / 3850.13 / 4128.11 | 11.631 / 12.430 / 13.469 ms | 58.575 ms | 90.0% / 90.0% / 90.0% | 100.00 MB |
| ON | 10 | 5092.32 / 5421.32 / 5920.65 | 14.417 / 15.748 / 17.072 ms | 84.655 ms | 90.0% / 90.0% / 90.0% | 200.00 MB |

### Eksperiment 4a — Replication Factor: write latencija i storage potrošnja (3+1 ponavljanja po RF, min / mean / max)

| RF | Write latencija min/mean/max | P95 latencija min/mean/max | Ukupno (logički) | Network estimate (tačno) | Storage izmjereno (mean) |
| --- | --- | --- | --- | --- | --- |
| 1 | 11.439 / 11.546 / 11.716 ms | 13.250 / 13.453 / 13.636 ms | 20.00 MB | 20.00 MB | 20.00 MB |
| 3 | 12.104 / 12.339 / 12.699 ms | 14.006 / 14.353 / 14.905 ms | 20.00 MB | 60.00 MB | 60.00 MB |

*Ponovo izmjereno 2026-09-23 nakon Faze 18 (paralelni fan-out upisa na
replike, `docs/design.md` §27). Prethodno mjerenje sa sekvencijalnim
upisom: RF=1 12.097 ms, RF=3 29.091 ms (mean). Eksperiment 4b nije
promijenjen (12.01 s / 40.02 s, identično).*

### Eksperiment 4b — Replication Factor: recovery ponašanje (3 ponavljanja po RF, min / mean / max)

| RF | Ishod (koliko od N ponavljanja oporavljeno) | Vrijeme do ishoda min/mean/max | detected_failures (Master, zbir) | successful_recoveries (Master, zbir) |
| --- | --- | --- | --- | --- |
| 1 | 0/3 OPORAVLJENO | 40.02 / 40.02 / 40.02 s | 3 | 0 |
| 3 | 3/3 OPORAVLJENO | 12.01 / 12.01 / 12.01 s | 3 | 3 |

### Eksperiment 5 — Storage Node Failure (3 ponavljanja po veličini fajla, min / mean / max)

| Veličina fajla | Failure detection vrijeme | Recovery trajanje | Bytes recovered (mean) | Finalno replication stanje |
| --- | --- | --- | --- | --- |
| 1.00 MB | 12.09 / 12.09 / 12.10 s (3/3) | 0.00 / 0.00 / 0.00 s (3/3) | 1.00 MB | 3/3 (posljednje ponavljanje) |
| 10.00 MB | 12.09 / 12.10 / 12.10 s (3/3) | 0.00 / 0.00 / 0.00 s (3/3) | 10.00 MB | 3/3 (posljednje ponavljanje) |

