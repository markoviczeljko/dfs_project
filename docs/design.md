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

## 5. Faza 1 — Network Protocol

**Payload limiti.** §5.17 pominje `MAX_CONTROL_MESSAGE_SIZE` i
`MAX_DATA_MESSAGE_SIZE` samo konceptualno, bez konkretnih vrednosti.
Usvojeno: `MAX_CONTROL_MESSAGE_SIZE = 64 KB` (dovoljno za metadata/LIST
odgovore), `MAX_DATA_MESSAGE_SIZE = BLOCK_SIZE + 4 KB` (pun 64 MB blok
plus mali overhead za block_id/checksum/veličinu polja).

**Prošireni `MessageType` enum.** §5.8 daje početnu listu tipova i
najavljuje da će se dopunjavati kasnije. Da bi wire format ostao
stabilan kroz sve naredne faze, enum je odmah proširen svim tipovima
pomenutim kroz §5/§6/§7/§8/§9: `REPLICATE_BLOCK`, `GET_BLOCK_INFO`
(§5.23), `WRITE_COMMIT`, `APPEND_COMMIT`, `COPY_BLOCK`, i `READ_OK`,
`NOT_MODIFIED`, `STALE_VERSION` (§9.8, conditional READ). Handleri za
većinu ovih tipova ne postoje još — samo je rezervisan slot u protokolu.

**Error-handling stil u network sloju.** §17.10 skicira `void
connect(...)` i `Socket accept()`. Umesto toga, `Socket::connect/bind/listen`
vraćaju `ErrorCode` (ne `void`), a `Socket::accept()` vraća `Result<Socket>`
— u duhu odluke iz Faze 0 da se greške eksplicitno signaliziraju, a ne
gutaju ćutke. `TCPConnection::sendMessage`/`receiveMessage` prate doslovne
signature iz §5.16 (`bool` / `std::optional<Message>`), pošto su tamo već
eksplicitno date.

**Payload strukture po operaciji nisu deo ove faze.** Dokument ne
definiše CREATE/READ/WRITE/APPEND/DELETE/LIST/STAT payload formate na
nivou bajtova nigde (§6/§7 opisuju tok, ne wire layout). Payload ostaje
opaque `std::vector<std::byte>` na nivou `Message` strukture; konkretni
formati se definišu u fazama koje implementiraju te operacije.

## 6. Faza 2 — BlockStorage

**Koja `BlockStorage` API verzija je usvojena.** Dokument sadrži tri
međusobno različita sketch-a (§4.7: camelCase + `std::optional`, §17.4:
snake_case + goli `vector`, §7: camelCase + `copyBlock`). Usvojena je
snake_case verzija iz §17.4 (`write_block`/`read_block`/`delete_block`/
`block_exists`), sa Faza 0/1 `ErrorCode`/`Result<T>` konvencijom umesto
doslovnog `bool`/goli `vector`, pošto nijedan sketch nije uzimao u obzir
tu (kasniju) odluku. `copyBlock` iz §7 je van obima — pripada replikaciji
(kasnija faza).

**Checksum algoritam: CRC-32.** Dokument nigde ne imenuje konkretan
algoritam. CRC-32 (IEEE 802.3 polinom) je dovoljan jer dokument
eksplicitno kaže da checksum služi samo za detekciju slučajne korupcije,
ne za bezbednost (§4.6).

**Dodatni fsync direktorijuma posle rename().** §4.10 opisuje
`write+fsync+rename`, ali ne pominje da i sam direktorijum treba
`fsync`-ovati da bi `rename()` bio garantovano trajan posle pada sistema
(POSIX zahtev). Dodato kao svesno poboljšanje u odnosu na doslovni tekst.

**Nema posebnog `local_index` fajla.** §4.3 prikazuje
`metadata/local_index` u strukturi direktorijuma, ali format nigde nije
definisan, a §4.15 recovery procedura eksplicitno radi pun disk-scan.
Odlučeno je da se ne uvodi nespecifikovan format — recovery je uvek
potpuno rekonstruisan skeniranjem `blocks/` fajlova.

**`.tmp` fajlovi se brišu pri recovery-ju**, ne samo ignorišu — sprečava
gomilanje zaostalih fajlova od prekinutih write operacija.
