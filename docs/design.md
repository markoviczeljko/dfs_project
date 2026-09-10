# Design Decisions — Faza 0

Ovaj dokument beleži implementacione odluke donete tokom Faze 0 (bootstrap),
uključujući rešavanje nedoslednosti pronađenih u internom dizajn dokumentu.

## 1. Error-handling konvencija

Klase u dizajn dokumentu nisu bile dosledne u signalizaciji grešaka
(neke vraćaju `bool`, neke goli `std::vector` bez načina da signaliziraju
specifičan uzrok greške). Birana je jedna konvencija za ceo projekat:
ručno pisan `Result<T, ErrorCode>` (`include/common/errors.h`).

Razlog: `std::expected<T, E>` je prirodan izbor za ovaj problem, ali je
C++23 feature — projekat je zaključan na C++20. `Result<T, ErrorCode>`
oponaša `std::expected` API (`is_ok`, `value()`, `error()`, `operator bool`)
kao drop-in upgrade put ako projekat ikad pređe na C++23.

C++ exceptions su rezervisane isključivo za nepopravljive/programer-greška
uslove (`bad_alloc`, invariant violations) — ne za očekivane fallible
operacije poput "fajl ne postoji".

## 2. StorageNodeId tip i ime

Dizajn dokument ima dve definicije: raniju (`NodeId`, `uint32_t`) i kasniju,
detaljniju (`StorageNodeId`, `uint64_t`). Usvojena je kasnija verzija —
`StorageNodeId = uint64_t` — jer je konzistentna sa `FileId`/`BlockId`
(oba `uint64_t`) i jer kasnije/detaljnije sekcije dokumenta imaju prioritet
nad ranijim skicama.

## 3. DEFAULT_REPLICATION_FACTOR tip

Dokument definiše ovu konstantu i kao `size_t` i kao `uint32_t` na dva
mesta. Usvojen je `uint32_t` — prirodniji tip za brojač replika i manji
footprint kad se kasnije bude slao kroz mrežni protokol.

## 4. Struktura foldera — odstupanje

Dizajn dokument predviđa da ceo kod živi u pod-folderu
`distributed-file-system/`. Pošto se git repo već zove `dfs_project`,
taj dodatni nivo ugnježdenosti je izostavljen — sav kod ide direktno u
koren repoa.
