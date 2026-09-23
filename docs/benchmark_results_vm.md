# DFS Benchmark Results — Realna Multi-VM Mreža

Generisano: 2026-09-19 14:26:53 UTC (ručno orkestrirano preko multipass/KVM VM-ova; vidi metodološke napomene ispod).

## Metodološke napomene

### Zašto ovaj dokument postoji

`docs/benchmark_results.md` mjeri sistem na loopback-u (Master i svi Storage
čvorovi kao odvojeni OS procesi na ISTOJ mašini, komunikacija preko 127.0.0.1).
Mentor je kao zamjerku naveo da to ne dokazuje da sistem radi preko prave
mreže i na fizički (virtuelno) odvojenim mašinama. Ovaj dokument ponavlja
IDENTIČNU metodologiju (iste veličine fajlova, isti broj klijenata, isto 3
ponavljanja po ćeliji, min/mean/max) protiv realnog multi-VM klastera, tako
da se rezultati mogu direktno uporediti red-po-red.

### Hardver i topologija

- Host mašina: Intel Core i7-13620H, 32 GB RAM, NVMe SSD, Ubuntu 24.04.3 LTS
  (ista mašina kao za loopback mjerenja).
- Virtuelizacija: multipass 1.16.4 (KVM backend).
- 5 virtuelnih mašina, Ubuntu 24.04.5 LTS: 1 Master VM (2 vCPU, 2 GB RAM) +
  4 Storage VM-a (1 vCPU, 4 GB RAM, 10 GB disk svaka).
- Svaka VM ima sopstvenu, realnu IP adresu na multipass-ovoj virtuelnoj mreži
  (10.182.251.0/24) i sopstveni virtuelni disk — komunikacija ide preko
  pravog TCP/IP steka (virtio-net), ne preko loopback prečice, a replike se
  fizički nalaze na odvojenim diskovima.
- **Poštena granica metodologije**: sve VM dijele isti fizički host (jedan
  laptop) — ovo NIJE test na fizički odvojenim mašinama, ali JESTE realan
  mrežni stek (TCP/IP preko virtuelnog switch-a) i realno odvojeni
  diskovi/memorija po VM-u, za razliku od loopback varijante gdje sve dijeli
  isti kernel i isti disk.
- Build tip: Release (`-DCMAKE_BUILD_TYPE=Release`), isti binarni fajlovi
  (dfs_master/dfs_storage/dfs_statsctl) prekopirani na svaku VM.
- Replication factor: Eksperimenti 1-3 i 5 korišćeni su protiv istog
  RF=3 klastera (4 Storage VM-a — 3 za replike + 1 rezervna za oporavak).
  Eksperiment 4 upoređuje RF=1 (isti 4 čvora, samo je master.conf
  rekonfigurisan i klaster restartovan sa čistim stanjem) naspram RF=3.

### Ponavljanja mjerenja

- Identično loopback metodologiji: svaka ćelija se izvršava 3 puta
  nezavisno, prikazuje se min / mean / max preko ta 3 ponavljanja.

### Adaptacije zbog udaljene (VM) topologije

- **Detekcija koji Storage čvor drži repliku** (Eksperimenti 4b i 5): pošto
  benchmark alat nema direktan pristup diskovima udaljenih VM-ova, umjesto
  `has_any_block_on_disk()` (loopback verzija) koristi se before/after
  razlika `used_bytes` polja iz GET_STATS odgovora svakog Storage čvora —
  mrežno-vidljiv ekvivalent iste provjere.
- **Master brojači (detected_failures/successful_recoveries/bytes) su
  kumulativni za cijeli životni vijek klastera** (klaster ostaje gore kroz
  sva ponavljanja, za razliku od loopback-a koji pravi svjež klaster po
  ponavljanju) — svako ponavljanje bilježi baseline vrijednost prije
  ubijanja čvora i traži pomak PREKO tog baseline-a, ne apsolutnu vrijednost.
- **Nakon svakog restarta ubijenog čvora, benchmark čeka da se ukupna
  storage potrošnja klastera stabilizuje** prije sljedećeg ponavljanja —
  čvor koji se vrati sa praznim diskom i dalje figuriše u Master-ovim
  metapodacima kao vlasnik svojih starih blokova (iz ranijih ponavljanja i
  Eksperimenta 4a), pa Master ispravno pokreće re-replikaciju tih blokova;
  ta legitimna aktivnost mora se stišati prije mjerenja sljedećeg
  ponavljanja da se ne pripiše pogrešnom ponavljanju (otkriveno i
  ispravljeno tokom sanity-testiranja ovog alata, vidi
  `bench/remote/remote_common.h`).
- Storage VM RAM je namjerno postavljen na 4 GB (ne manje) — inicijalni
  pokušaj sa 1.5 GB je rezultovao OOM-killed `dfs_storage` procesom
  usred Eksperimenta 1 (do 10 konkurentnih klijenata × 256 MB blok drži se
  u memoriji po in-flight READ-u), potvrđeno preko `dmesg`/`journalctl -k`.
- **Poznato ograničenje: "Bytes recovered" kolona u Eksperimentu 5 nije
  pouzdana kao u loopback verziji.** Pošto isti klaster ostaje gore kroz
  Eksperimente 1-5 (bez svježeg restarta po eksperimentu), Master-ov
  kumulativni `Recovery: bytes=` brojač u trenutku mjerenja može uključivati
  bajtove iz DRUGE, istovremene re-replikacije koju je pokrenulo ranije
  ubijanje/restart nekog čvora (vidi napomenu o stabilizaciji iznad) — u
  ovom run-u to je vidljivo kao 12.00 MB za 1 MB fajl i 6.33 MB za 10 MB
  fajl, umjesto tačnog poklapanja koje loopback verzija pokazuje. Vrijeme
  detekcije otkaza i stopa uspješnog oporavka (3/3 za obje veličine) ostaju
  pouzdani signali — oni ne zavise od ovog istog brojača.

### Eksperiment 1 (VM) — Sequential READ (3 ponavljanja po ćeliji, min / mean / max)

| Veličina fajla | Broj klijenata | Throughput min/mean/max (MB/s) | Latencija min/mean/max | P95 latencija (pooled) |
| --- | --- | --- | --- | --- |
| 1 KB | 1 | 2.45 / 3.04 / 4.17 | 0.205 / 0.301 / 0.363 ms | 0.771 ms |
| 1 KB | 5 | 10.28 / 13.63 / 15.33 | 0.246 / 0.298 / 0.373 ms | 1.102 ms |
| 1 KB | 10 | 21.76 / 23.07 / 24.93 | 0.332 / 0.340 / 0.344 ms | 1.492 ms |
| 64 KB | 1 | 94.31 / 166.18 / 228.11 | 0.246 / 0.395 / 0.612 ms | 0.927 ms |
| 64 KB | 5 | 600.08 / 631.71 / 658.37 | 0.393 / 0.409 / 0.418 ms | 1.299 ms |
| 64 KB | 10 | 711.82 / 804.42 / 883.60 | 0.518 / 0.549 / 0.566 ms | 0.691 ms |
| 1 MB | 1 | 576.64 / 612.74 / 658.02 | 1.482 / 1.595 / 1.690 ms | 1.338 ms |
| 1 MB | 5 | 1471.02 / 1546.90 / 1604.00 | 3.003 / 3.124 / 3.268 ms | 1.525 ms |
| 1 MB | 10 | 1870.65 / 1981.25 / 2066.74 | 4.354 / 4.659 / 5.053 ms | 2.510 ms |
| 10 MB | 1 | 923.70 / 950.59 / 965.34 | 10.098 / 10.235 / 10.493 ms | 5.395 ms |
| 10 MB | 5 | 1509.60 / 1616.93 / 1735.55 | 27.976 / 29.575 / 30.436 ms | 22.225 ms |
| 10 MB | 10 | 1676.21 / 1854.54 / 2018.69 | 45.740 / 49.448 / 53.002 ms | 36.603 ms |
| 64 MB | 1 | 537.27 / 565.62 / 582.49 | 106.665 / 109.646 / 115.104 ms | 106.633 ms |
| 64 MB | 5 | 1434.80 / 1522.85 / 1585.54 | 195.378 / 204.183 / 216.758 ms | 133.217 ms |
| 64 MB | 10 | 1724.49 / 1756.77 / 1788.25 | 342.947 / 350.816 / 359.159 ms | 259.971 ms |
| 128 MB | 1 | 569.60 / 577.03 / 581.80 | 212.317 / 214.233 / 217.250 ms | 185.583 ms |
| 128 MB | 5 | 1358.61 / 1481.78 / 1562.72 | 395.530 / 419.786 / 456.252 ms | 266.008 ms |
| 128 MB | 10 | 1447.35 / 1702.19 / 1867.38 | 655.623 / 729.790 / 847.312 ms | 454.438 ms |
| 256 MB | 1 | 568.07 / 571.96 / 575.36 | 425.173 / 427.688 / 430.733 ms | 343.082 ms |
| 256 MB | 5 | 1565.94 / 1619.90 / 1674.40 | 713.889 / 744.652 / 762.441 ms | 487.296 ms |
| 256 MB | 10 | 1907.97 / 1945.17 / 2016.04 | 1160.941 / 1188.526 / 1218.741 ms | 1187.623 ms |


### Eksperiment 2 (VM) — Random READ (3 ponavljanja po ćeliji, min / mean / max)

| Broj klijenata | Throughput min/mean/max (MB/s) | Latencija min/mean/max | P95 latencija (pooled) | Cache hit ratio min/mean/max |
| --- | --- | --- | --- | --- |
| 1 | 104.42 / 113.42 / 121.51 | 8.192 / 8.804 / 9.516 ms | 11.383 ms | 13.3% / 17.8% / 23.3% |
| 5 | 294.69 / 309.79 / 319.07 | 14.024 / 14.571 / 15.617 ms | 32.968 ms | 14.7% / 16.7% / 18.7% |
| 10 | 431.93 / 446.69 / 456.69 | 19.441 / 20.351 / 21.751 ms | 51.965 ms | 15.7% / 17.8% / 20.7% |


### Eksperiment 3 (VM) — Cache ON vs OFF (3 ponavljanja po ćeliji, min / mean / max)

| Cache | Broj klijenata | Throughput min/mean/max (MB/s) | Latencija min/mean/max | P95 latencija (pooled) | Cache hit ratio min/mean/max | Aproks. network/disk (mean) |
| --- | --- | --- | --- | --- | --- | --- |
| OFF | 1 | 105.67 / 107.21 / 109.58 | 91.224 / 93.260 / 94.599 ms | 112.215 ms | 0.0% / 0.0% / 0.0% | 200.00 MB |
| OFF | 5 | 222.58 / 232.58 / 237.67 | 202.673 / 209.365 / 220.648 ms | 336.989 ms | 0.0% / 0.0% / 0.0% | 1000.00 MB |
| OFF | 10 | 245.75 / 246.72 / 248.42 | 392.415 / 394.304 / 396.271 ms | 645.031 ms | 0.0% / 0.0% / 0.0% | 2000.00 MB |
| ON | 1 | 700.76 / 735.11 / 757.79 | 13.159 / 13.576 / 14.223 ms | 104.014 ms | 90.0% / 90.0% / 90.0% | 20.00 MB |
| ON | 5 | 1063.56 / 1076.91 / 1101.98 | 43.722 / 44.225 / 44.524 ms | 352.634 ms | 90.0% / 90.0% / 90.0% | 100.00 MB |
| ON | 10 | 1207.13 / 1224.00 / 1247.16 | 71.519 / 73.961 / 75.234 ms | 651.223 ms | 90.0% / 90.0% / 90.0% | 200.00 MB |


### Eksperiment 4a (VM) — Replication Factor: write latencija i storage potrošnja (3 ponavljanja, min / mean / max) — RF=3

| RF | Write latencija min/mean/max | P95 latencija min/mean/max | Ukupno (logički) | Network estimate (tačno) | Storage izmjereno (mean, per-repeat delta) |
| --- | --- | --- | --- | --- | --- |
| 3 | 72.847 / 75.445 / 78.515 ms | 78.693 / 82.543 / 87.035 ms | 20.00 MB | 60.00 MB | 60.00 MB |

### Eksperiment 4b (VM) — Replication Factor: recovery ponašanje (3 ponavljanja, min / mean / max) — RF=3

| RF | Ishod (koliko od N ponavljanja oporavljeno) | Vrijeme do ishoda min/mean/max |
| --- | --- | --- |
| 3 | 3/3 OPORAVLJENO | 11.62 / 12.13 / 12.64 s |


### Eksperiment 4a (VM) — Replication Factor: write latencija i storage potrošnja (3 ponavljanja, min / mean / max) — RF=1

| RF | Write latencija min/mean/max | P95 latencija min/mean/max | Ukupno (logički) | Network estimate (tačno) | Storage izmjereno (mean, per-repeat delta) |
| --- | --- | --- | --- | --- | --- |
| 1 | 31.246 / 32.771 / 34.635 ms | 35.927 / 37.558 / 40.647 ms | 20.00 MB | 20.00 MB | 20.00 MB |

### Eksperiment 4b (VM) — Replication Factor: recovery ponašanje (3 ponavljanja, min / mean / max) — RF=1

| RF | Ishod (koliko od N ponavljanja oporavljeno) | Vrijeme do ishoda min/mean/max |
| --- | --- | --- |
| 1 | 0/3 OPORAVLJENO | 40.41 / 40.42 / 40.44 s |


### Eksperiment 5 (VM) — Storage Node Failure (3 ponavljanja po veličini fajla, min / mean / max)

| Veličina fajla | Failure detection vrijeme | Recovery trajanje | Bytes recovered (mean) |
| --- | --- | --- | --- |
| 1.00 MB | 12.13 / 12.47 / 12.63 s (3/3) | 0.00 / 0.17 / 0.50 s (3/3) | 12.00 MB |
| 10.00 MB | 11.11 / 11.79 / 12.13 s (3/3) | 0.00 / 0.17 / 0.50 s (3/3) | 6.33 MB |


