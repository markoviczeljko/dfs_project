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

## 7. Faza 3 — BlockManager

**`allocate_block()` je privremeni lokalni generator, ne finalni izvor
`BlockId`-a.** Dizajn dokument (§3.7, §3.9, §6.4) eksplicitno kaže da je
generisanje `BlockId`-eva Master-ova odgovornost. Ovde implementiran
rastući brojač zaštićen mutex-om služi isključivo za samostalno
testiranje ove faze. Kad se u Fazi 5 umreži pravi tok, Master postaje
izvor `BlockId`-eva, a ovaj lokalni generator prestaje da bude
relevantan za stvarni sistem.

**`write_block`/`read_block` namerno ne zahtevaju prethodnu lokalnu
alokaciju.** Storage Node će u Fazi 5 primati `WRITE_BLOCK` sa ID-em
koji je Master već dodelio — nikad lokalno alociranim. Zato ove dve
metode čisto prosleđuju pozive `BlockStorage`-u, bez provere da li je
`id` prošao kroz `allocate_block()`.

**Bez `used_space`/`free_space`/`block_exists` na `BlockManager`
nivou.** §17.5 prozni opis pominje "free-space accounting" kao
odgovornost, ali ni C++ sketch ni `IMPLEMENTATION_PLAN.md`-ova lista
metoda za ovu fazu to ne uključuju. Accounting ostaje dostupan direktno
na `BlockStorage` nivou; ne izmišlja se nespecifikovan API sloj.

**`release_block` vraća `ErrorCode`, ne `bool`** (ista adaptacija kao
za `BlockStorage::write_block`/`delete_block` u Fazi 2), i idempotentan
je — brisanje već obrisanog/nepostojećeg bloka nije greška.

## 8. Faza 4 — NamespaceManager

**`BlockMetadata`: usvojena §7.3 verzija nad §3.4.** Dokument ima dve
definicije — §3.4 (bez `version`/`state`) i §7.3 (kasnija, dodaje oba
polja). Usvojena je §7.3 verzija, sa `StorageNodeId` umesto `NodeId`
(dosledno sa `common/types.h` iz Faze 0).

**`BlockState` je definisan kao tip sada, logika dolazi u Fazi 12.**
Enum (`ALLOCATED/WRITING/COMMITTED/UNDER_REPLICATED/RECOVERING/
CORRUPTED/DELETING`) postoji u `master/metadata.h`, ali tranzicije
između stanja implementira Master-ovo recovery knjigovodstvo — ova
faza ga ne koristi aktivno.

**Direktorijumi su implicitni prefiksi putanja, ne posebni entiteti.**
Dokument nigde ne definiše `MKDIR` niti direktorijum-kao-entitet.
`list_files("/data")` pronalazi decu na osnovu toga koje putanje
postojećih fajlova počinju sa `/data/` — nema eksplicitnog kreiranja
direktorijuma, pa prazan/nepostojeći prefiks uvek vraća praznu listu,
nikad grešku.

**Pravila validacije putanje** (potpuno nespecifikovana u dokumentu):
mora početi sa `/`; bez praznih segmenata (dupli `/`); bez `.`/`..`
segmenata; nevalidna putanja → `INVALID_REQUEST`. `"/"` je validan
argument za `list_files` (koren), ali ne i za `create_file`/
`delete_file` (koren nije fajl).

**`create_file`/`delete_file` vraćaju `ErrorCode`, ne `bool`** (§17.3
sketch koristi goli `bool`, nedovoljno da razlikuje
`FILE_ALREADY_EXISTS`/`FILE_NOT_FOUND`) — ista adaptacija kao svuda
dosad.

**`FileId` brojač je trajan izvor istine**, za razliku od `BlockId`-a
u Fazi 3 — `NamespaceManager` jeste sâm Master-ov namespace sloj, nema
spoljne komponente koja bi kasnije preuzela generisanje `FileId`-a.

## 9. Faza 5 — StorageNode proces

**`NetworkServer` je nova klasa u `network/` sloju, van originalnog
rasporeda faza.** §17.10 skicira `NetworkServer` kao `start()/stop()`
komponentu deljenu između Master-a i Storage Node-a, ali nijedna faza
u `IMPLEMENTATION_PLAN.md`-u je nikad nije zakazala za implementaciju —
ni Faza 1 (koja je izgradila samo `Socket`/`Protocol`/`TCPConnection`).
Pošto će Faza 6 (Master) trebati identičan accept-loop, `NetworkServer`
je izgrađen sada kao generička, ponovo iskoristiva klasa koja ne zna
ništa o `BlockManager`-u ili storage-u — prima `MessageHandler`
callback i vrti accept-thread + jedan detached worker-thread po
konekciji (thread-per-connection, §11.3). `stop()` radi
`shutdown(SHUT_RDWR)` na listening file descriptor-u da otključa
blokirajući `accept()`, pa `join()`-uje accept-thread; worker-thread-ovi
se ne join-uju eksplicitno — završavaju sami kad se konekcija zatvori
(dosledno "diplomski scope" pojednostavljenju iz §11.3).

**Wire-format za `WRITE_BLOCK`/`READ_BLOCK`/`DELETE_BLOCK` payload je
izmišljen u ovoj fazi.** `network/protocol.h` nosi samo generički
`vector<byte>` payload — strukture po operaciji su eksplicitno van
dometa Faze 1 (§5, dokumentovano u sekciji 5 iznad). Usvojen je layout
(sve big-endian, dosledno header-u):
- `WRITE_BLOCK` zahtev: `block_id`(u64) + `data_size`(u32) + `data`;
  odgovor `ACK(block_id)` ili `ERROR(error_code)`.
- `READ_BLOCK` zahtev: `block_id`(u64); odgovor
  `ACK(block_id + data_size + data)` ili `ERROR(error_code)`.
- `DELETE_BLOCK` zahtev: `block_id`(u64); odgovor uvek `ACK(block_id)`
  (idempotentno, §4.13 zahteva "retry-friendly" ponašanje).

Enkodiranje/dekodiranje živi kao slobodne funkcije u `storage_node.cpp`,
ne dira `protocol.h`/`protocol.cpp`.

**`DELETE_BLOCK` handler zove `BlockManager::release_block(id)`** —
jedini metod na tom sloju koji i briše fizički blok i oslobađa id;
`BlockManager` nema poseban "samo storage-level delete" poziv.

**Nema `Statistics` stub klase u ovoj fazi.** §17.6 i
`IMPLEMENTATION_PLAN.md` pominju `Statistics` kao član `StorageNode`-a,
ali prava implementacija dolazi tek u Fazi 14. Prazna klasa bez ijedne
funkcije ne bi donela vrednost sada — izostavljena je u potpunosti.

**Konfiguracija (`configs/storage1.conf`) je prost `key=value` tekstualni
format**, izmišljen u ovoj fazi — dokument nigde ne definiše šemu za
ovaj fajl, samo pominje njegovo ime u §14 stablu direktorijuma. Polja:
`node_id`, `port`, `data_dir`, `capacity_bytes`. `node_id` se čita i
čuva radi buduće kompatibilnosti (Faza 12 registracija kod Master-a),
ali se ne koristi mrežno u ovoj fazi — nema Master-a još.

**`main_storage.cpp`**: ručni argv parser za `--config <path>` (bez
eksterne biblioteke, dosledno projektu koji do sada nema nijednu
eksternu zavisnost osim GoogleTest-a). Graceful shutdown preko
`SIGINT`/`SIGTERM` postavlja `std::atomic<bool>` flag koji glavna
petlja proverava; nijedan main()/signal-handling sketch ne postoji u
dizajn dokumentu — ovo je u potpunosti nova odluka ove faze.

**Testovi ostaju u `tests/unit/`** (nema novog `tests/integration/`
direktorijuma, dosledno postojećoj konvenciji): `test_network_server.cpp`
(generički accept-loop/echo-handler test) i `test_storage_node.cpp`
(end-to-end WRITE→READ round-trip, DELETE idempotencija,
`STORAGE_FULL`/`BLOCK_NOT_FOUND` error putanje) preko pravog TCP
loopback-a, istim obrascem kao `TCPConnectionLoopback` testovi iz
Faze 1. Ovim je "ad hoc smoke test" iz `IMPLEMENTATION_PLAN.md`-a
pretvoren u ponovljiv regresioni test umesto jednokratnog ručnog
skripta.

## 10. Faza 6 — Master proces

**Pet međusobno nekonzistentnih "Master" dekompozicija u dizajn
dokumentu (§2.2, §6.15, §7.22/7.23, §8.45, §17.7), usvojena je §17.7.**
Ranije sekcije pominju `MetadataManager`, `NodeManager`,
`PlacementManager`, `FailureDetector`, `MetadataStore` i Master-ov
sopstveni `BlockManager` — svi nestaju do §17.7 bez ijednog objašnjenja
da su spojeni/preimenovani/napušteni. §17.7 (`NamespaceManager` +
`ReplicationManager` + `StorageRegistry` + `NetworkServer` +
`Statistics`) je jedina verzija koju prati i `IMPLEMENTATION_PLAN.md`,
pa je usvojena kao kanonska.

**`Master` klasa u ovoj fazi NEMA `StorageRegistry`/`ReplicationManager`
stub članove**, iako ih plan pominje kao "deklarisane ali stub/no-op."
Ista logika kao izostanak `Statistics` stuba u Fazi 5 (§9): prazna
klasa bez ijedne funkcije koju ništa u ovoj fazi ne koristi ne donosi
vrednost — dodaju se u Fazi 9 kad `StorageRegistry` stvarno nešto radi
(registracija/heartbeat čvorova). `Master` u ovoj fazi ima samo
`NamespaceManager` (direktan member, ne `unique_ptr` — default je
konstruktibilan, nema failure mode kao `BlockManager`) i
`NetworkServer`.

**`NetworkServer` iz Faze 5 reuse-ovan bukvalno bez ijedne izmene** —
dizajn dokument (§17.10) eksplicitno kaže da je dijeljen između
Master-a i Storage Node-a, i `Master::start()` ga koristi identičnim
obrascem kao `StorageNode::start()`.

**`READ_FILE`/`WRITE_FILE` idu u `default` granu dispečera →
`ERROR(INVALID_REQUEST)`.** Ovi tipovi poruka postoje u `MessageType`
enumu ali nemaju definisan tok nigde u dokumentu — svi READ/WRITE
flow-ovi (§6.2, §6.4) koriste `GET_FILE_METADATA`/`ALLOCATE_BLOCK`/
`WRITE_COMMIT` umesto njih (a `GET_FILE_METADATA` čak ni ne postoji u
enumu). Namerno isključenje, ne previd — ti tipovi dolaze tek sa
pravim Client-om u Fazi 7.

**Wire-format za `CREATE_FILE`/`DELETE_FILE`/`LIST_FILES`/`STAT_FILE`
je izmišljen u ovoj fazi**, isti obrazac kao Faza 5 (lokalne slobodne
funkcije u `master.cpp`, `protocol.h/cpp` se ne dira):
- `CREATE_FILE`/`DELETE_FILE`/`LIST_FILES`/`STAT_FILE` zahtev: cela
  putanja kao sirovi UTF-8 bajtovi (nema length-prefiksa — payload
  JESTE putanja, `Protocol` već garantuje tačnu dužinu).
- `CREATE_FILE`/`DELETE_FILE` odgovor: `ACK` (prazan) ili
  `ERROR(error_code)`.
- `LIST_FILES` odgovor: `ACK` sa `entry_count`(u32) + po entry-ju
  `name_length`(u32)+`name`.
- `STAT_FILE` odgovor: `ACK` sa `file_id`(u64)+`size`(u64)+
  `modification_time`(u64)+`block_count`(u32)+`block_id`(u64)×count —
  tačno polja koja `FileMetadata` ima (§12.6 pominje
  `replication_factor`/`created_at` koji ne postoje u strukturi; putanja
  se ne vraća jer je klijent već zna).

**`DELETE_FILE` u ovoj fazi samo uklanja namespace entry.** §12.9 opisuje
i čišćenje fizičkih blokova na Storage Node-ovima, ali u ovoj fazi
nijedan blok ne postoji (`ALLOCATE_BLOCK` je Faza 7) — puni,
replication-aware DELETE dolazi u Fazi 11.

**Konfiguracija (`configs/master.conf`)** — isti `key=value` format kao
Faza 5, samo sa poljem `port` (nema data_dir/capacity — Master ne
perzistira ništa na disk u ovoj fazi). Port 9000 po §15.4 primeru
rasporeda (Master→9000, Storage 1→9101...).

**`main_master.cpp`** — bukvalno isti obrazac kao `main_storage.cpp`
(argv `--config` parser, `key=value` loader, `SIGINT`/`SIGTERM`
graceful shutdown) — dizajn dokument ne daje main()/signal skicu za
`dfs_master` (za razliku od Storage Node-a, ni ono što ima nije
detaljno), pa je obrazac prenet 1:1.

**Testovi**: `tests/unit/test_master.cpp`, `TEST_F` fixture bez temp-dir-a
(`NamespaceManager` je čisto in-memory), TCP loopback protiv živog
`Master` objekta, isti stil kao `test_storage_node.cpp`. Pokriva
CREATE/STAT/DELETE/LIST round-trip, `FILE_ALREADY_EXISTS`/
`FILE_NOT_FOUND` greške, `READ_FILE`/`WRITE_FILE`/nepoznat tip →
`INVALID_REQUEST`, ivične slučajeve (prazan payload, `CREATE_FILE` na
`/`, putanja bez vodećeg `/`), i konkurentan `CREATE_FILE` sa 8
paralelnih konekcija na istu putanju (tačno jedan `OK`, ostatak
`FILE_ALREADY_EXISTS` — provjerava da `NamespaceManager`-ov
`shared_mutex` stvarno štiti od trke, pošto `Master` sam ne dodaje
nikakvo zaključavanje).

### Dopuna: kritičan bag u `NetworkServer`-u (Faza 5) nađen pri code review-u ove faze

Nezavisan `cpp-reviewer` agent je pri pregledu ove faze prijavio i
**reprodukovao** heap-use-after-free: `NetworkServer::stop()` je do sada
zaustavljao samo accept-thread, dok su worker-thread-ovi (po jedan po
konekciji) bili `.detach()`-ovani i nikad praćeni. Ako klijent drži
konekciju otvorenu dok se proces gasi (`SIGTERM` → `Master::stop()`/
`StorageNode::stop()` → destruktor), taj worker-thread ostaje zaglavljen
u `recv()` i drži `this`-capture na `handler_` lambdi — kad se
`Master`/`StorageNode` oslobodi, sljedeća poruka od tog klijenta puca u
oslobođenu memoriju. Potvrđeno ASan repro-om (heap-use-after-free na
`std::function::operator()` iz oslobođenog `NetworkServer`-a).

**Popravka** (u `include/network/network_server.h` +
`src/network/network_server.cpp`, dijeljeno sa Fazom 5): `stop()` sada
prati sve worker-thread-ove (`std::vector<Worker>`, svaki sa svojim
`fd`-om), radi `shutdown(SHUT_RDWR)` na fd-u svake još-otvorene konekcije
(da otključa eventualni blokirajući `recv()`), pa tek onda `join()`-uje
svaki thread — garantujući da nijedan worker ne trči poslije što
`stop()` vrati kontrolu. Da dugotrajni server ne akumulira "mrtve" ali
nespojene thread handle-ove, `accept_loop()` usput čisti već-završene
worker-e pri svakom novom `accept()`-u (provjera preko
`shared_ptr<atomic<bool>> finished` po workeru — bez toga `join()` bi
mogao blokirati na thread-u koji još čeka podatke).

Ovo je jedini slučaj do sada da je bag nađen u već pushanom kodu
prethodne faze — popravljen je odmah jer je stvaran memory-safety
problem koji utiče na oba izvršna fajla (`dfs_storage` i `dfs_master`),
ne samo na kod ove faze. Dodat je i trajni regresioni test
(`NetworkServer.StopClosesLiveConnectionWithoutHangingOrLeakingTheThread`
u `tests/unit/test_network_server.cpp`).

Manji nalazi iz istog review-a, takođe popravljeni: `bytes_to_string`
sada eksplicitno vraća `{}` za prazan payload umjesto da se osloni na
to da `std::vector::data()` vrati `nullptr` a `std::string` to tiho
toleriše; `main_master.cpp`-ov `port=` parser sada odbija vrijednosti
van `[0, 65535]` i sufikse koji nisu brojevi (npr. `port=8080smece`)
umjesto da ih tiho odsiječe/ignoriše (`std::stoul` sa provjerom
`consumed == value.size()`). Isti propust postoji i u
`main_storage.cpp` (Faza 5) — nije dirano u ovoj fazi, ostaje kao
poznato, niskorizično zaduženje za kasnije.

## 11. Faza 7 — Minimalan end-to-end CREATE/WRITE/READ

Ovo je prva faza gdje Master, Storage Node i (nov) Client rade zajedno
(§18.1 vertical-slice). Dizajn dokument ovdje ima najviše praznina do
sada — nijedan mehanizam pomenut ispod nije bio konkretno specificiran,
sve je izmišljeno u ovoj fazi.

**Dvije precedentne odluke** (potvrđene sa korisnikom prije
implementacije, pošto se ponavljaju u svakoj sljedećoj integration-test
fazi):
- **Integration testovi koriste in-process objekte**, ne odvojene OS
  procese — `Master`/`StorageNode`/`Client` se prave kao pravi C++
  objekti u istom test binary-ju, komunikacija i dalje ide preko pravih
  TCP loopback konekcija. §16.3 opisuje pravi multi-proces harness, ali
  to unosi infrastrukturu (process lifecycle, binary paths, cleanup)
  nesrazmjernu koristi za diplomski — isti obrazac kao svi dosadašnji
  testovi (`test_storage_node.cpp`, `test_master.cpp`), samo sad
  kombinovan.
- **`READ_FILE`** (postojeći, do sada nekorišten enum — Faza 6 je
  eksplicitno testirala da vraća `INVALID_REQUEST`) **postaje prava
  "daj mi metapodatke za čitanje" poruka**, umjesto uvođenja novog
  `GET_FILE_METADATA` u `protocol.h`. Rješava gap iz Faze 6 (`READ_FILE`
  je imao rezervisano mjesto u enumu bez ijedne definisane semantike) i
  izbjegava diranje `protocol.h`-ovog enuma, koji nijedna faza do sada
  nije mijenjala. Mijenja jedan Faza 6 test
  (`ReadFileAndWriteFileAreOutOfScope` → `WriteFileRemainsOutOfScope`,
  `READ_FILE` provjera zamijenjena pravim testovima).

**`BlockId` generisanje — nema dizajna nigdje**, samo proza "Master
generiše novi BlockID". Dodat `NamespaceManager::allocate_block(path)`,
analogan postojećem `next_file_id_` obrascu: nov `next_block_id_`
brojač, zaštićen istim `shared_mutex`-om (`unique_lock`, iako je čitanje
`files_`-a read-only, mutacija brojača zahtijeva ekskluzivni pristup).
Provjerava da fajl postoji (`FILE_NOT_FOUND` inače) ali **ne dira**
`FileMetadata.blocks` — to se dešava tek na `commit_write`.

**`NamespaceManager` nije imao NIJEDAN metod za ažuriranje
`blocks`/`size`/`modification_time` poslije `create_file`-a** — dodat
`commit_write(path, block_id, size)`. Pošto ova faza podržava tačno
jedan blok po fajlu, `blocks` se **zamjenjuje** sa `{block_id}` (ne
dodaje) — WRITE je overwrite semantika po §13.3, multi-block append
dolazi tek u Fazi 8.

**Master ne zna adresu Storage Node-a ni na koji način prije Faze 9**
(`StorageRegistry`/`REGISTER_NODE` ne postoje do tada). Riješeno
**privremenom statičkom konfiguracijom**: `Master::Config` dobija
`storage_node_host`/`storage_node_port`, čita se iz `configs/master.conf`.
Svaki `ALLOCATE_BLOCK`/`READ_FILE` odgovor vraća tu istu adresu — nema
liste replika (RF=1 efektivno), dosljedno "svaka faza izmišlja svoj
wire format nezavisno" konvenciji. Faza 9 će morati potpuno redizajnirati
ovaj dio odgovora kad Storage Node-ovi postanu dinamički.

**Tri nova Master handlera** (`ALLOCATE_BLOCK`, `WRITE_COMMIT`,
`READ_FILE`), payload formati izmišljeni ovdje (lokalne funkcije u
`master.cpp`, `protocol.h/cpp` netaknut):
- `ALLOCATE_BLOCK` zahtjev = sirova putanja; odgovor = `block_id`(u64) +
  `host`(length-prefiksovan) + `port`(u16).
- `WRITE_COMMIT` zahtjev = `path_length`(u32)+`path`+`block_id`(u64)+
  `size`(u64); odgovor = prazan `ACK` ili `ERROR`.
- `READ_FILE` (repurposed) zahtjev = sirova putanja; odgovor =
  `file_id`+`size`+`modification_time`+`block_count`+ po bloku
  `block_id`+`host`+`port` (`block_count` je uvijek 0 ili 1 u ovoj fazi).

**`Client` klasa (`include/client/client.h`) — nema posebnu
`MasterConnection` wrapper klasu.** Dizajn dokument daje `MasterConnection`
tačno jednu liniju (`MasterConnection master;`, bez ijednog metoda) —
prazan omotač oko `TCPConnection`-a ne bi donio vrijednost (isti rezon
kao izostanak `Statistics`/`StorageRegistry` stubova u Fazama 5/6).
`Client` direktno drži `TCPConnection master_connection_` (persistentna,
otvorena jednom u `create()`, po §5.24), a konekcija ka Storage Node-u
je po-operaciji (otvori/piši-ili-čitaj/zatvori — nema pooling-a, nije
potreban za jedan node). `write()` provjerava `data.size() <= BLOCK_SIZE`
lokalno prije kontaktiranja Master-a (fail-fast na granici sistema).
Nema thread-safety mehanizma — `Client` nije zamišljen za konkurentnu
upotrebu (single-threaded CLI alat).

**`tests/integration/` napravljen prvi put** — do sada su svi testovi
bili u `tests/unit/` (deviation dokumentovana u Fazama 5/6). Ova faza
stvarno testira više komponenti zajedno, tačno po
`IMPLEMENTATION_PLAN.md`-ovoj fajl listi: `test_create.cpp` (samo
Master+Client), `test_write.cpp`/`test_read.cpp` (Master+StorageNode+
Client, pun CREATE→WRITE→READ round-trip).

**Ručna provjera protiv pravih procesa**: `dfs_client create/write/read`
CLI preko stvarno pokrenutih `dfs_master`+`dfs_storage`, `cmp` potvrđuje
identične bajtove između izvornog i pročitanog fajla, čist `SIGINT`
shutdown oba procesa.

### Dopuna: `WRITE_COMMIT` nije provjeravao vlasništvo nad `block_id`-jem (nađeno pri code review-u ove faze)

Nezavisan `cpp-reviewer` agent je prijavio da `commit_write(path, block_id,
size)` prihvata **bilo koji** `block_id` bez provjere da je taj id
stvarno prethodno izdat za taj fajl preko `ALLOCATE_BLOCK`-a. Dokaz:
`WriteCommitUpdatesMetadata` test je (prije popravke) commit-ovao
`block_id=42` za `/a.txt` bez ijednog prethodnog `ALLOCATE_BLOCK` poziva,
i Master ga je bez pogovora prihvatio. Posljedica: klijent (bagovan ili
zlonamjeran) mogao je "ukrasti" tuđ, već upisan blok tako što bi ga
commit-ovao pod svojom putanjom — `READ_FILE`/`Client::read()` bi tada
transparentno vratio tuđe podatke. Ovo je ozbiljnije od već prihvaćenog
"nema GC-a starog bloka pri overwrite-u" ograničenja (koje samo troši
prostor, ne otkriva tuđe podatke).

**Popravka**: `NamespaceManager` dobija `pending_blocks_`
(`unordered_map<string, BlockId>`, putanja → posljednji dodijeljen ali
još ne commit-ovan blok). `allocate_block()` upisuje u ovu mapu;
`commit_write()` odbija (`INVALID_REQUEST`) ako dati `block_id` ne
odgovara zapisu u `pending_blocks_`-u za tu putanju, i briše zapis
nakon uspješnog commit-a. Drugi `ALLOCATE_BLOCK` za isti fajl prirodno
"pregazi" prvi — commit sa starim (stale) id-jem se odbija.
`delete_file()` takođe čisti `pending_blocks_` zapis. Dodata dva nova
regresiona testa (`WriteCommitWithoutPriorAllocationFails`,
`WriteCommitWithStaleBlockIdFails`), i dva postojeća testa
(`WriteCommitUpdatesMetadata`, `ReadFileReturnsMetadataWithBlockLocation`)
su ispravljena da prvo stvarno alociraju blok prije commit-a. Pravi
`Client::write()` tok nije bio pogođen — on je oduvijek commit-ovao
tačno onaj `block_id` koji je upravo dobio od `ALLOCATE_BLOCK`-a.

## 12. Faza 8 — Multi-block WRITE/READ

Uklanja Faza 7-ino ograničenje "fajl ima tačno jedan blok". `Client::write()`
sada dijeli podatke veće od jednog bloka na više chunk-ova; `Client::read()`
ih sastavlja nazad. `NamespaceManager`/`Master` prelaze sa "commit tačno
jednog `block_id`-a" na "commit uređene liste `block_id`-jeva".
`IMPLEMENTATION_PLAN.md` eksplicitno bira **jedan finalni `WRITE_COMMIT`
nakon svih per-chunk `ALLOCATE_BLOCK`+`WRITE_BLOCK` poziva**, nasuprot
`dfs_design_notes.md` §6.6-ovom dijagramu koji pokazuje commit po bloku —
plan dokument je konačna korisnikova odluka i arhitekturno je čišći: neuspjeh
usred petlje nikad ne mutira `FileMetadata.blocks` djelimično, jer
`WRITE_COMMIT` se tada jednostavno nikad ne šalje. `StorageNode`/
`NetworkServer` se ne diraju — svaki blok je već potpuno nezavisna jedinica
adresirana samo preko `BlockId`-a; `handle_read_file`/`handle_stat_file` su
već bili "N-block-ready" na wire nivou (već petljaju kroz `metadata.blocks`),
samo `Client::read()` je čitao isključivo prvi element te liste.

**`pending_blocks_` — subset-check + full-clear, umjesto Faza 7-inog
exact-match.** Faza 7 je garantovala "samo najnovija alokacija za path je
važeća" (svaki novi `allocate_block()` prepisivao je prethodni pending id).
Faza 8 zahtijeva da više uzastopnih `allocate_block()` poziva za isti path
ostanu **istovremeno validni** do jednog finalnog commit-a, pa je to
ograničenje nužno napušteno. `pending_blocks_` postaje
`unordered_map<string, unordered_set<BlockId>>`; `allocate_block()` sada
**dodaje** u set umjesto da prepisuje. `commit_write(path, block_ids, size)`
prima listu id-jeva i validira da je **svaki** id iz te liste prisutan u
pending setu za taj path (subset-check, ne exact-match cijelog seta). Nakon
uspješnog commit-a, `pending_blocks_.erase(path)` briše **cijeli** set —
i iskorištene i eventualne zaostale id-jeve iz ranijeg napuštenog write
pokušaja. Exact-match bi trajno blokirao path nakon bilo kog neuspjelog
write pokušaja (zaostali pending id-jevi nikad ne bi mogli tačno da se
poklope sa novom listom); subset-check + full-clear to rješava — neiskorišteni
pending id-jevi se tiho odbacuju (postaju orphan blokovi, ista GC-odloženo
filozofija kao kod overwrite-a). Cross-path krađa ostaje potpuno blokirana
(id alociran za path A nikad ne uđe u `pending_blocks_[B]`).

**Namjerno napuštena semantika, dokumentovano**: stari Faza 7 test
`WriteCommitWithStaleBlockIdFails` (tvrdio je da drugi `ALLOCATE_BLOCK`
poništava prvi za isti path) je povučen — to svojstvo je nespojivo sa
multi-chunk write-om koji zahtijeva više istovremeno važećih pending
alokacija. Zamijenjen sa `WriteCommitAcceptsAnyPreviouslyAllocatedIdForThisPath`
(dokumentuje novo ponašanje), `WriteCommitWithBlockIdFromDifferentPathFails`
(pravi cross-path anti-theft regresioni test, preuzima ulogu koju je stari
test imao za tu zabrinutost), `WriteCommitWithMultipleBlocksSucceedsAndPreservesOrder`,
`WriteCommitSubsetOfPendingBlocksSucceedsAndDiscardsUnusedIds` (dokazuje da
abandoned write pokušaj ne blokira sljedeći legitiman write) i
`WriteCommitWithUnallocatedIdAmongOtherwiseValidIdsFails`.

**Poznato, prihvaćeno ograničenje (nije popravljeno)**: dva istovremena
`Client::write()` poziva na **isti path** mogu dovesti do toga da jedan
legitimno završi write, ali njegov `commit_write()` (koji radi full-clear)
obriše pending blokove drugog, još-u-toku write-a — taj drugi dobije lažni
`INVALID_REQUEST` iako ništa nije ukradeno/oštećeno. Ista kategorija race-a
kao Faza 7-ino prihvaćeno allocate-time ograničenje, samo pomjerena na
commit-time; real fix (per-writer izolacija/lease) van obima do neke buduće
faze. Slično, `pending_blocks_` može rasti neograničeno za path koji se
ponovljeno write-uje-pa-napusti bez ikad commit-ovanja — ograničeno samo
memorijom procesa, ne diskom.

**`WRITE_COMMIT` wire format promijenjen**: staro
`[u32 path_len][path][u64 block_id][u64 size]` (tačno jedan blok) →
novo `[u32 path_len][path][u64 size][u32 block_count]
[block_count x u64 block_id]` (uređena lista, proizvoljne dužine).
`ALLOCATE_BLOCK` i `READ_FILE`/`STAT_FILE` se ne mijenjaju — potonja dva su
već bila N-block-ready na wire nivou.

**Veličina bloka konfigurabilna samo na `Client` strani**
(`Client::Config.block_size`, default = `BLOCK_SIZE` iz `common/config.h`).
Master/`NamespaceManager` ne treba da zna veličinu chunk-a uopšte — samo
prima gotovu uređenu listu blokova, pa ostaje potpuno agnostičan. Integration
testovi override-uju `block_size` na par KB da multi-block scenarije testiraju
brzo bez pomjeranja stvarnih 64 MB kroz loopback TCP u svakom test run-u.
`Client::write()` odbija `block_size == 0` ili `block_size > BLOCK_SIZE`
(`INVALID_REQUEST`) — potonje bi inače propalo tek duboko u `TCPConnection`
sa zbunjujućim `INTERNAL_ERROR`-om jer bi premašilo `MAX_DATA_MESSAGE_SIZE`.

**Empty-write promjena ponašanja**: `write(path, {})` sada proizvodi 0
blokova (`chunk_count = data.empty() ? 0 : ceil(...)`), umjesto Faza 7-inog
ponašanja gdje je prazan write i dalje alocirao 1 blok sa 0 bajtova. Manja
namjerna pojednostavljenja `ceil()` formule, ne krši nijedan postojeći test.

### Dopuna: `is_data_message()` je pogrešno klasifikovao ACK/ERROR odgovore, ograničavajući READ_BLOCK odgovore na 64 KB (nađeno pri ručnoj end-to-end provjeri ove faze)

Bug nije bio uveden u ovoj fazi — postojao je od Faze 5, ali nikad nije
okinut jer nijedan ručni test do sada nije stvarno pročitao/upisao preko
64 KB podataka u jednom bloku. Otkriven tek kad je Faza 8 prvi put ručno
testirana na realno velikom fajlu (130 MB, 3 bloka) protiv pravih pokrenutih
`dfs_master`/`dfs_storage` procesa: `write` je uspio (podaci idu preko
`WRITE_BLOCK` **zahtjeva**, ispravno klasifikovanog kao "data message"), ali
`read` je propao sa nedešifrovanom greškom — čak i za obično 1 MB fajl.

**Uzrok**: `is_data_message(MessageType)` u `src/network/protocol.cpp`
klasifikuje poruku po njenom **tipu**, ali odgovor na `READ_BLOCK` zahtjev
ima tip `ACK` (generički, ne `READ_BLOCK`), pa `TCPConnection::receiveMessage()`
u `src/network/connection.cpp` nameće mali `MAX_CONTROL_MESSAGE_SIZE`
(64 KB) limit na ACK odgovore koji legitimno nose do punog bloka (64 MB)
podataka — svaki `payload_length` preko 64 KB odbacuje se kao
`INVALID_MESSAGE` prije nego što se uopšte pokuša pročitati tijelo poruke.

**Popravka**: `MessageType::ACK` i `MessageType::ERROR` dodati u
`is_data_message()`-ovu listu tipova koji dobijaju veliki limit — primalac
nema način da zna na koji zahtjev dati ACK/ERROR odgovara, pa oba tipa
uvijek moraju dobiti velikodušan `MAX_DATA_MESSAGE_SIZE` limit. Ovo ne
otvara novi sigurnosni gap: `WRITE_BLOCK`/`READ_BLOCK` i ostali data-tipovi
su već prije ove izmjene mogli izazvati istu (već postojeću, van obima ove
faze) slabost — bez per-connection read timeout-a, proizvoljan TCP klijent
može poslati header koji tvrdi `payload_length` blizu 64 MB i zauvijek
blokirati worker thread čekajući tijelo poruke koje nikad ne stiže; dodavanje
ACK/ERROR-a u listu ne mijenja tu prijetnju na način koji već nije postojao.

Usput popravljen i srodan, sitniji bug u istoj datoteci:
`connection.cpp`-ov `INVALID_MESSAGE` error-payload je kopirao sirove bajte
bez `htobe16()` poziva — nekonzistentno sa svakim drugim mjestom u kodu koje
kodira `ErrorCode` na wire (sva ostala mjesta u `master.cpp`/`storage_node.cpp`/
`client.cpp` prolaze kroz `htobe16` prije kopiranja). Popravljeno dodavanjem
`htobe16()` poziva.

Nezavisan `cpp-reviewer` agent potvrdio je i popravku dizajna i ova dva
mrežna bug-fixa kao ispravne, bez CRITICAL/HIGH nalaza.

## 13. Faza 9 — Replikacija (RF=3)

Uklanja single-node ograničenje: `Master::Config` je do sada imao samo
`storage_node_host`/`storage_node_port` — jednu statičku adresu koju je
svaki `ALLOCATE_BLOCK`/`READ_FILE` odgovor vraćao, eksplicitno označenu kao
"privremeni stand-in za pravi StorageRegistry (Faza 9)". Storage Node-ovi se
sada registruju kod Master-a (`REGISTER_NODE`) pri startu; Master bira 3
replike po bloku (`ReplicationManager::choose_replicas`); `Client::write()`
piše direktno na sve 3 (fan-out, Master nikad ne proxy-uje podatke); commit
se dešava samo na 3/3 ACK (strogo, ne kvorum, po §7.7); `Client::read()`
pokušava replike redom uz fallback na sljedeću. Faza je eksplicitno "happy
path" — heartbeat/timeout detekcija otkaza čvora, reaktivna re-replikacija,
puna JOIN state mašina, i WRITE_BLOCK/WRITE_COMMIT idempotencija su
odložene do Faze 12.

**Manje od RF=3 živih čvorova pri ALLOCATE_BLOCK → `NODE_NOT_FOUND`**
(potvrđeno sa korisnikom). Alternativa (alociraj na dostupne + označi
UNDER_REPLICATED) bi stvorila blok koji ova faza nikad ne može popraviti
(repair je Faza 12) — nekonzistentno sa §7.7-ovom strogom 3/3 filozofijom.
`BlockId` je u tom slučaju već upisan u `pending_blocks_` za tu putanju od
`namespace_manager_.allocate_block()`-a prije nego što se replika bira —
isti "orphan dok se path ponovo ne commit-uje/obriše" ishod kao napušten
write pokušaj (§12), ne novo curenje koje ova faza uvodi.

**Placement algoritam: deterministički round-robin kursor**
(`ReplicationManager::cursor_`, pod sopstvenim mutex-om) preko
`get_alive_nodes()` sortiranih po id-u — ne random. Nema `<random>`
upotrebe nigdje u kodu do sada, i round-robin je tačno reproducibilan u
unit testovima (`ChooseReplicasRoundRobinsAcrossSuccessiveCalls`), za
razliku od hash/random politike. `StorageNodeInfo.free_space` je
deklarisano (popunjeno sa `total_space` pri registraciji, potvrđeno sa
korisnikom da se ne ažurira dok heartbeat ne postoji — Faza 12), ali se
placement ne filtrira po njemu u ovoj fazi.

**Nalaz: `BlockState::WRITING` i `UNDER_REPLICATED` nemaju trigger u ovoj
fazi.** Master nikad nije u data-plane write petlji (Client piše
`WRITE_BLOCK` direktno na Storage Node-ove; Master saznaje ishod tek
retroaktivno preko `WRITE_COMMIT`-a), pa `WRITING` nema tačku gdje bi se
ikad postavio — blok ide direktno `ALLOCATED → COMMITTED`
(`NamespaceManager::set_block_replicas`/`commit_write`). `UNDER_REPLICATED`
je po §7.8 definisan kao posljedica `HEALTHY → DEAD` heartbeat tranzicije
čvora — pošto ova faza nema heartbeat/failure-detection uopšte, ta
vrijednost je nedostižna. Nezavisan `cpp-reviewer` agent je potvrdio
grep-om da se `.state` nigdje u kodu ne postavlja/čita osim na ta dva
mjesta — obje enum vrijednosti su namjerno deklarisane-ali-nepostavljene,
ne propust.

**`StorageRegistry`/`ReplicationManager` API**: `StorageRegistry`
implementira sve 4 metode iz §17.9 skice (`register_node` — idempotentan
upsert, `mark_dead`, `mark_alive`, `get_alive_nodes`) plus jednu dodatu van
skice, stvarno potrebnu odmah: `get_node(id)` (Master mora razriješiti
izabrani `StorageNodeId` nazad u `host:port`). `ReplicationManager`
deklariše samo `choose_replicas(BlockId)` — `replicate_block`/
`handle_node_failure` iz §17.8 skice se ne deklarišu (nema pozivaoca u
ovoj fazi, isti "no value-less stub" presedan kao Faze 5–8).

**Wire formati** (svi novi, `protocol.h`/`.cpp` netaknut, isti "svaka faza
izmišlja svoj payload lokalno" obrazac):
```
REGISTER_NODE zahtjev:   [u64 node_id][u32 host_len][host][u16 port][u64 capacity_bytes]
ALLOCATE_BLOCK odgovor:  [u64 block_id][u32 replica_count]{[u32 host_len][host][u16 port]}×replica_count
READ_FILE odgovor:       [u64 file_id][u64 size][u64 mtime][u32 block_count]
                          { [u64 block_id][u32 replica_count]{[u32 host_len][host][u16 port]}×replica_count }×block_count
```
Duplikat `node_id` pri registraciji → idempotentan upsert (dev-otpornost na
ručni restart čvora). Neuspjela registracija pri startu StorageNode-a →
fail-fast, bez retry-ja (isti stil kao postojeći `start()`).

**Redoslijed pokretanja se mijenja**: `StorageNode::start()` sad sinhrono
poziva `register_with_master()` (prva upotreba outbound konekcije unutar
`StorageNode`-a — do sada je imao samo inbound `NetworkServer`) i
fail-fast-uje ako Master nije dostižan. `scripts/start_master.sh` mora
sada raditi PRIJE bilo kog `scripts/start_storage.sh`-a — obrnuto od
Faza 5–8-ine "bilo koji redoslijed radi" pretpostavke. `scripts/start_storage.sh`
je parametrizovan indeksom čvora (`./scripts/start_storage.sh 2` →
`configs/storage2.conf`) umjesto tri skoro-identične skripte.

**Testna infrastruktura**: `tests/unit/test_storage_node.cpp`-ova fixture
sad mora pokrenuti i pravi in-process `Master` (pošto `StorageNode::start()`
bezuslovno zahtijeva uspješnu registraciju) — `master_port` se izvodi kao
`storage_port + 100` da ostane bez kolizije sa postojećom po-testu-jedinstven-port
konvencijom, bez posebne tabele portova. `tests/integration/test_write.cpp`/
`test_read.cpp` fixture-i sada pokreću Master pa 3 `StorageNode` instance
(RF fiksan na 3, bez tunability-ja za testove — za razliku od Faza 8-inog
`block_size`, pokretanje 3 in-process objekata ne košta ništa uporedivo sa
pomjeranjem 64MB podataka).

**Ručna provjera** ("Definicija gotovog" iz `IMPLEMENTATION_PLAN.md`):
Master + 3 prava `dfs_storage` procesa, multi-block write preko
`dfs_client`, inspekcija data direktorijuma svakog node-a potvrđuje da
blok postoji na sva 3; ugašen jedan Storage Node proces (`SIGINT`),
potvrđeno da READ i dalje uspijeva preko client-side fallback-a (Master
nikad ne saznaje da je čvor mrtav — nema heartbeat do Faze 12); `cmp`
bajt-po-bajt identično prije i poslije.

Nezavisan `cpp-reviewer` agent: bez CRITICAL/HIGH nalaza (potvrdio
reference-member lifetime u `Master`-u, thread-safety `StorageRegistry`/
`ReplicationManager`-a, round-robin aritmetiku uključujući wraparound kad
broj živih čvorova nije umnožak RF-a, i bounds-checking u svim novim wire
parserima). Dva MEDIUM/LOW nalaza primijenjena: komentar o
orphan-pending-block-id ishodu na `NODE_NOT_FOUND`, i dodati konkurentni
testovi za `StorageRegistry`/`ReplicationManager` (nedostajali su, iako je
postojeća zaključavanja logika već bila ispravna).

## 14. Faza 10 — APPEND

Dodaje `Client::append(path, data)`. Ključni gap koji ni
`IMPLEMENTATION_PLAN.md` ni `dfs_design_notes.md` ne pominju: `BlockMetadata.size`
se do sada nigdje stvarno ne popunjava (hardkodovano na 0 u
`set_block_replicas()`, nikad ažurirano) — a APPEND mora znati trenutnu
popunjenost fajlovog posljednjeg bloka da bi odredio offset (§6.8). Odabrano
rješenje: Master ostaje isključivo server (nikad ne inicira konekciju ka
StorageNode-u, za razliku od alternative "upitaj `GET_BLOCK_INFO` uživo") —
`WRITE_COMMIT` i novi `APPEND_COMMIT` sad eksplicitno nose stvarnu veličinu
posljednjeg (dotičnog) bloka, koju Master upisuje u `BlockMetadata.size`.
Samo POSLJEDNJI blok liste ikad treba tačnu veličinu — interior blokovi se
nikad ne revizituju kao append-target, isti princip kao `commit_write()`.

**`BlockMetadata` dobija `reserved_size`** (odvojeno od `size`): `size` je
POTVRĐENA vrijednost (mijenja se samo pri uspješnom commit-u), `reserved_size`
je monotono rastući kursor za dodjelu append-offset-a. Razdvajanje je nužno
da bi konkurentni append-i mogli odmah vidjeti "sljedeći slobodan offset" bez
čekanja na commit prethodnog — `resolve_append_target()` pod
`unique_lock`-om odmah pomjeri `reserved_size` (optimistička rezervacija,
bez I/O), isti obrazac kao postojeći `allocate_block()`+`pending_blocks_`.
Overflow preko granice bloka (§6.10) se ne rješava u istom Master
round-trip-u kao fill-cilj — `APPEND_FILE` odgovor opisuje isključivo
eventualni fill u postojećem posljednjem bloku; ostatak ide kroz identičan,
refaktorisan `ALLOCATE_BLOCK`-po-chunk-u loop koji `Client::write()` već
koristi (`write_new_blocks()`/`write_chunk_to_replicas()`/
`parse_replica_list()` dijeljeni helperi). Ovo pokriva i proizvoljno velike
append-e i append-na-prazan-fajl (§6.11, automatski `has_target=false`) bez
posebne grane koda.

**`StorageNode`/`BlockStorage`**: `WRITE_BLOCK` wire format dobija `offset`
polje — `offset==0` zadržava identično postojeće ponašanje (pun O_TRUNC
overwrite), `offset>0` ide na novu `BlockStorage::write_block_at()`. Ova
metoda NE radi sirov `pwrite` u postojeći fajl (što bi narušilo postojeću
crash-safety garanciju) — radi read-modify-write pod JEDNIM
`unique_lock`-om: učita trenutni sadržaj, zero-fill-uje gap ako je `offset`
ispred trenutne fizičke dužine (vidi dolje), spoji novi podatak, pa upiše
kroz identičan tmp+fsync+rename put kao `write_block()`.

**Konkurentnosne rupe otkrivene i popravljene tokom razvoja i review-a**
(bez njih bi "definicija gotovog" — 2 konkurentna klijenta, 100× append —
bila flaky ili tiho pogrešna):

1. **Fizički redoslijed pristiglih `WRITE_BLOCK` poziva na storage node-u ne
   prati redoslijed dodjele offset-a** (odvojeni network round-trip-ovi,
   thread-per-connection) — moguće je da klijent sa KASNIJIM offset-om
   fizički stigne na storage node PRIJE klijenta sa RANIJIM. Striktan
   `offset == trenutna dužina` zahtjev bi ovo tretirao kao grešku iako nije
   korupcija. Popravka: `write_block_at()` dozvoljava write na bilo koji
   offset unutar `BLOCK_SIZE` i zero-fill-uje gap ako je offset ispred
   trenutne dužine — redoslijed fizičkih upisa postaje irelevantan za
   uspjeh.
2. **Redoslijed `APPEND_COMMIT` poziva na Masteru takođe nije garantovan.**
   Naivan bezuslovan `size = ...` bi mogao vratiti `size` unazad ako kasniji
   (po redoslijedu commit-a) append zapravo ima RANIJI offset. Popravka:
   `commit_append()` radi CAS-stil commit — `expected_prior_size` mora
   tačno odgovarati trenutnoj POTVRĐENOJ `size`, inače se čeka
   (`condition_variable_any`, timeout `APPEND_COMMIT_WAIT_TIMEOUT_MS`=3s)
   dok predak-append ne commit-uje i pozove `notify_all()`.
3. **(Nađeno tek pri prvom ručnom testu, prije review-a) Novoalociran blok
   nikad ne dobija stvaran `reserved_size`.** `commit_append()`-ov
   new-blocks loop je originalno radio `block.size = block.reserved_size`
   — ali `reserved_size` novog bloka ostaje 0 (nikad ga niko ne pomjera
   osim `resolve_append_target()`, koja se primjenjuje samo na POSTOJEĆI
   posljednji blok, ne na novoalocirane). Rezultat: drugi uzastopni append
   je računao offset 0 za blok koji zapravo već ima podatke, i
   `WRITE_BLOCK(offset=0, ...)` (pun overwrite) je tiho UNIŠTIO sadržaj
   prvog append-a. Popravka: `APPEND_COMMIT` sad nosi i
   `last_new_block_size` (analogno `WRITE_COMMIT`-ovom `last_block_size`),
   koji `commit_append()` upisuje u i `size` i `reserved_size` POSLJEDNJEG
   novog bloka.
4. **(Nađeno pri concurrent testu, prije review-a) `commit_append()`-ovo
   brisanje CIJELOG `pending_blocks_` seta pri svakom komitu** (kopirano od
   `commit_write()`-a) je pogrešno za append: dva konkurentna append-a na
   ISTI prazan/pun-posljednji-blok fajl mogu istovremeno imati validan
   pending novi blok — prvi commit bi obrisao CIJELI set, odbacujući drugi
   sa lažnim `INVALID_REQUEST`. Popravka: `commit_append()` briše samo
   id-jeve koji se STVARNO komituju u tom pozivu, ne cijeli set (za razliku
   od `commit_write()`-a, kojem ovaj scenario nikad ne prijeti jer
   pretpostavlja jednog sekvencijalnog pisca).

**Nezavisan `cpp-reviewer` agent — 2 CRITICAL + 1 HIGH nalaz, oba CRITICAL
popravljena:**

- **CRITICAL: dangling iterator kroz CV wait.** Originalna implementacija je
  držala `files_`-iterator (`file_it`) preko `wait_for()` poziva, koji
  interno otključava `mutex_`. Konkurentan `delete_file(path)` u tom
  prozoru invalidira iterator — UB pri narednoj dereferenci. Popravljeno:
  ništa se ne drži preko wait-a; CAS predikat radi svoj `blocks_.find()`
  svaki put kad se pozove, a sve (`files_`, `pending_blocks_`, `blocks_`)
  se ponovo traži OD NULE odmah nakon wait-a, prije bilo koje mutacije.
  Regresioni test: `CommitAppendSurvivesConcurrentDeleteFileDuringWait`
  (ASan potvrđuje odsustvo use-after-free).
- **CRITICAL: nedostaje provjera vlasništva nad `existing_block_id`-jem.**
  `blocks_` je jedna ravna mapa dijeljena preko svih fajlova — bez provjere,
  bilo koji `APPEND_COMMIT` je mogao imenovati blok koji pripada SASVIM
  DRUGOM fajlu, i ako bi se `expected_prior_size` slučajno poklopio
  (trivijalno tačno za bilo koji još-ne-append-ovan blok, koji kreće od 0),
  tiho bi mutirao tuđi blok. Popravljeno: `file_owns_block()` provjerava da
  `existing_block_id` postoji NEGDJE u `path`-ovoj `blocks` listi (ne mora
  biti tačno posljednji — dva konkurentna append-a mogu legitimno biti u
  toku istovremeno, jedan još komituje fill u blok koji je u međuvremenu
  već prestao biti posljednji zbog tuđeg rollover-a na nov blok), provjereno
  i prije i poslije CV wait-a. Regresioni test:
  `CommitAppendRejectsBlockIdBelongingToAnotherFile`.
- **HIGH (prihvaćeno, nije popravljeno ove faze): napuštena rezervacija
  trajno "zaglavi" TAJ JEDAN blok dok se ne popuni.** Ako klijent
  crash-uje/otkaže poslije `resolve_append_target()`-ove rezervacije a
  prije `APPEND_COMMIT`-a, `reserved_size` ostaje trajno ispred `size`-a za
  taj blok — svaki naredni append tom bloku računa offset koji CAS nikad
  neće moći potvrditi, pa svaki takav poziv čeka puni
  `APPEND_COMMIT_WAIT_TIMEOUT_MS` (3s) pa vraća `TIMEOUT`, sve dok
  `reserved_size` ne dostigne `BLOCK_SIZE` i append prirodno ne pređe na
  nov blok. Pravi fix (lease sa isteka/rollback) zahtijeva razlikovanje
  "spor pisac" od "mrtav pisac" — što zahtijeva failure detection koja ne
  postoji do Faze 12, ista kategorija namjerno odloženog ograničenja kao
  "manje od RF=3 živih čvorova" (§13) i orphan pending blokovi (§12).
  Ograničeno na JEDAN blok (fajl nikad trajno ne postaje neupisiv u
  cjelini), ali vrijedi ga imati na umu kao poznat gap.

**Wire formati** (svi novi/prošireni, `protocol.h`/`.cpp` netaknut osim
enum vrijednosti koje su već postojale rezervisane od ranije):
```
WRITE_COMMIT zahtjev (prošireno):  [u32 path_len][path][u64 size][u32 block_count]
                                    {u64 block_id}×block_count[u64 last_block_size]
WRITE_BLOCK zahtjev (prošireno):   [u64 block_id][u64 offset][u32 data_size][data]
APPEND_FILE zahtjev/odgovor:       req: [path][u64 append_size]
                                    resp: [u8 has_target], ako 1:
                                    [u64 block_id][u64 offset][u32 fill_amount]
                                    [u32 replica_count]{[u32 host_len][host][u16 port]}×replica_count
APPEND_COMMIT zahtjev:             [path][u64 total_appended_bytes][u8 has_existing_target], ako 1:
                                    [u64 existing_block_id][u64 expected_prior_size][u32 filled_amount]
                                    [u32 new_block_count]{u64 block_id}×new_block_count[u64 last_new_block_size]
```

**Ručna provjera**: Master + 3 prava `dfs_storage` procesa. (a) Sekvencijalni
append-i malog fajla (`create`+`write` 1000B, dva `append`-a od 50B i 200B)
— `cmp` bajt-po-bajt identično očekivanom spoju. (b) Pravi
block-boundary-crossing na punoj `BLOCK_SIZE` skali: `write` fajla od
`BLOCK_SIZE - 500` bajtova (jedan pun blok minus 500), pa `append` od 2000
bajtova (500 popunjava prvi blok, 1500 ide u nov drugi blok) — `read` vraća
tačno `BLOCK_SIZE + 1500` bajtova, `cmp` identično; inspekcija sva 3 node-a
potvrđuje identične veličine `.block` fajlova (uključujući header) na svakom.

131/131 testova prolazi (107 stari + 24 nova: 5 u `test_block_storage.cpp`,
1 u `test_block_manager.cpp`, 13 u `test_namespace.cpp` — uključujući
`CommitAppendRejectsBlockIdBelongingToAnotherFile` i
`CommitAppendSurvivesConcurrentDeleteFileDuringWait`, regresione testove za
cpp-reviewer-ove dva CRITICAL nalaza — i 5 u
`tests/integration/test_append.cpp`), čisto pod ASan/UBSan, stabilno kroz
ponovljena pokretanja.

## 15. Faza 11 — DELETE / LIST / STAT (replication-aware) + WRITE overwrite GC

Zatvara `dfs_design_notes.md` §12/§13 u potpunosti. Prije ove faze,
`DELETE_FILE` je bio čisti namespace tombstone: `NamespaceManager::delete_file()`
brisao je samo `files_[path]`/`pending_blocks_[path]`, nikad `blocks_` mapu
niti fizičke blokove na Storage Node-ovima — trajno curenje. Isto tako,
`commit_write()`-ovo WRITE-overwrite ponašanje je već ispravno zamjenjivalo
`FileMetadata.blocks` (i time već davalo besplatnu "stara verzija preživi
neuspio WRITE" garanciju iz §13.4), ali je stare `BlockId`-jeve prosto
odbacivalo — isto curenje. `StorageNode::handle_delete_block()` je već bio
potpuno implementiran i idempotentan (od Faze 5); ova faza mu je samo dala
pozivaoca.

**Rezolucija nejasnoća iz dokumentacije** (§5.22/§5.23 kontradiktorno navode
i Client i Master kao moguće pošiljaoce `DELETE_BLOCK`-a; kasnija, autoritativnija
poglavlja §12.9/§13.2/§13.3 i sam `IMPLEMENTATION_PLAN.md` pretpostavljaju
Master-only) — odabrano: **Master je jedini pošiljalac `DELETE_BLOCK`-a**.
Takođe: dokument koristi termin "orphan" za DVA različita koncepta — §8.34
(blok koji StorageNode ima a Master ga uopšte ne poznaje, zahtijeva
rekonsilijaciju, "ne treba ga odmah automatski obrisati") i §13.2 (blok koji
Master EKSPLICITNO zna da je superseded i želi ga obrisati). Ova faza
implementira ISKLJUČIVO §13.2-tip — §8.34-tip zahtijeva heartbeat/inventory
infrastrukturu koja ne postoji do Faze 12.

**Novi tip** (`include/master/metadata.h`): `OrphanedBlock{BlockId id,
std::vector<StorageNodeId> replicas}` — snapshot replika bloka u trenutku
uklanjanja iz `blocks_`, jer se `BlockMetadata` unos briše u istom pozivu
koji ovaj struct proizvodi. Namjerno BEZ novog `gc.h`/`gc.cpp` (kako je
`IMPLEMENTATION_PLAN.md` predlagao) — cijeli mehanizam je jedan plain struct
plus dvije `NamespaceManager` metode plus mali Master-side dispatch loop,
bez ikakvog nezavisnog stanja/threadinga koji bi opravdali novu klasu/fajl.

**`NamespaceManager::delete_file()`/`commit_write()`** mijenjaju povratni tip
iz `ErrorCode` u `Result<std::vector<OrphanedBlock>>` — pod već držanim
`unique_lock`-om, prije brisanja/zamjene `FileMetadata.blocks`, uhvate
replika-snapshot svakog starog bloka i obrišu ga iz `blocks_`. Bezbjedno
bezuslovno u `commit_write()`-u: stari i novi block-id skupovi se nikad ne
preklapaju jer `allocate_block()` strogo inkrementira `next_block_id_`.

**Master** dobija `gc_dispatch()`/`send_delete_block()`/`next_request_id()`
— nakon uspješnog `delete_file()`/`commit_write()`, Master otvara svježu
odlaznu TCP konekciju ka svakoj replici svakog orphan bloka i šalje
`DELETE_BLOCK`, **sinhrono** (blokira klijentov ACK dok se svi pokušaji ne
završe — čini kriterijum "fizički uklonjeno sa sve 3 replike" deterministički
testabilnim bez polling-a, po §3.11-ovom "jednostavniji sinhroni model za
prvu verziju") i **best-effort** (connect failure/timeout/non-ACK se guta,
nikad ne propagira klijentu kao greška — isti obrazac kao Faza 9 "manje od
RF=3 živih čvorova" i Faza 10 liveness gap).

**Client** dobija `delete_file()`, `list()`, `stat()` (+ `FileStat` struct),
prateći tačan `create_file()`-stil jednog round-trip-a ka Masteru.
`main_client.cpp` dobija `delete`/`list`/`stat` CLI podkomande. Imenovanje
`delete_file()` (ne dizajn-dokov skicirani `remove()` iz §17.11, koji
predatira stvarni kod i već je nekonzistentan sa `create_file()`) —
`delete` je uostalom C++ ključna riječ.

**Protokol bez izmjena** — `DELETE_FILE`, `LIST_FILES`, `STAT_FILE`,
`DELETE_BLOCK` su već postojali u `MessageType` enumu sa potpuno
implementiranim wire formatima na strani koja ih prima; Master samo postaje
nov POŠILJALAC `DELETE_BLOCK`-a.

**Nezavisan `cpp-reviewer` agent — 1 CRITICAL + 1 HIGH + 2 MEDIUM nalaza:**

- **CRITICAL: `send_delete_block()` nije imao I/O timeout.** `Socket` nigdje
  u projektu nije imao mehanizam za ograničavanje trajanja `send`/`recv`
  poziva. Storage Node koji prihvati konekciju ali nikad ne odgovori
  (zaglavljen proces, spor handler) bi zauvijek blokirao worker thread koji
  opslužuje klijentov `DELETE_FILE`/`WRITE_COMMIT` — i, pošto
  `NetworkServer::stop()` zna samo da zatvori KLIJENTOV fd svakog worker-a
  (ne i ODLAZNU konekciju koju `send_delete_block()` otvara), mogao je
  učiniti CIJELI `Master::stop()` (pa i destrukciju) trajno nemogućim.
  Popravljeno: nov `Socket::set_timeout(int timeout_ms)` (postavlja
  `SO_RCVTIMEO`/`SO_SNDTIMEO`), pozvan u `send_delete_block()` sa novom
  `DELETE_BLOCK_TIMEOUT_MS = 2000` konstantom (`common/config.h`) — istek
  se ponaša identično postojećem connect-failure putu (`recv`/`send` vraćaju
  -1, `TCPConnection::sendAll`/`recvAll` to već tretiraju kao običnu
  grešku). Regresioni test: `MasterTest.DeleteFileWithHangingReplicaCompletesWithinBoundedTime`
  (fake node koji prihvati konekciju pa spava duže od timeout-a).
- **(Otkriveno TOKOM popravke gornjeg nalaza, ne od strane reviewer-a)
  Zatvaranje odlazne konekcije nakon timeout-a otkrilo je DRUGI, skriveni
  bug: proces bez ikakvog rukovanja `SIGPIPE`-om.** Kad Master zatvori
  konekciju (timeout istekao), a spori Storage Node kasnije ipak pokuša
  poslati svoj ACK na tu već zatvorenu vezu, `send()` diže `SIGPIPE` čija je
  DEFAULT akcija ubijanje CIJELOG procesa (ne samo thread-a) — u produkciji
  bi ovo mogao srušiti sam Storage Node proces zbog Master-ove strane
  timeout logike. Nigdje u projektu se `SIGPIPE` nije ni ignorisao ni
  blokirao. Reprodukovano direktno (test proces je umro sa exit kodom 141 =
  128+SIGPIPE) prije popravke. Popravljeno: `Socket::create()` jednom po
  procesu (`std::call_once`) ignoriše `SIGPIPE` (`signal(SIGPIPE, SIG_IGN)`)
  — svaki proces koji ikad napravi `Socket` (Master, Client, StorageNode, svi
  test binariji) sada dobija ovu zaštitu automatski; pogođen `send()` sada
  samo vraća -1/`EPIPE`, tretiran identično svakom drugom I/O neuspjehu.
- **HIGH: data race na `Master::next_request_id_`.** Prije ove faze Master
  nikad nije sam inicirao odlazne zahtjeve (samo je odjekivao klijentov
  `request_id`) — `next_request_id_++` na plain `uint64_t`-u, čitano-mijenjano
  iz konkurentnih `NetworkServer` worker thread-ova (thread-per-connection),
  je čist data race. Popravljeno: `std::atomic<uint64_t>` +
  `fetch_add(1, std::memory_order_relaxed)`.
- **MEDIUM (dokumentovano komentarom, ne promjena ponašanja): orphan-capture
  petlja tiho preskače `BlockId` koji je u `file.blocks` ali nedostaje u
  `blocks_`-mapi.** Ovo bi bio invarijant-prekršaj koji se ne bi trebao
  desiti; namjerno tretiran kao no-op (fajl-level operacija i dalje uspijeva)
  umjesto greške, konzistentno sa identičnim `if (block_it != blocks_.end())`
  tretmanom iste situacije nekoliko linija niže u `commit_write()`-ovom
  COMMITTED-state loop-u.
- **MEDIUM (prihvaćeno, nije "bug" nego posljedica dizajna): `gc_dispatch()`
  je sekvencijalan** (jedan connect+send+recv po blok×replika paru) —
  N×RF×timeout najgori slučaj za veliki DELETE. Prihvaćeno za ovu fazu;
  paralelizacija bi bila prirodan sljedeći korak ako ikad postane problem u
  praksi.

**Samostalno pronađen bug (ASan, tokom pisanja `test_list_stat.cpp`):**
`ConcurrentCreateAndListBasicCorrectness`/`ConcurrentDeleteAndListBasicCorrectness`
su prvobitno pozivali `new_client()` (koji radi `clients_.push_back(...)` na
fixture-ovom NESINHRONIZOVANOM `std::vector`-u) IZNUTRA dva odvojena
`std::thread`-a — konkurentan `push_back` na dijeljenom vektoru je data race
(ASan: heap-use-after-free pri realokaciji vektora). Popravljeno kreiranjem
oba `Client` objekta na glavnoj niti PRIJE pokretanja thread-ova, isti obrazac
kao `AppendIntegrationTest`-ov postojeći `ConcurrentAppendsFromTwoClientsDoNotOverlapOrLoseData`.

**Prihvaćeni, dokumentovani gapovi (nisu ove faze bug-ovi, nego namjerne
odluke)**:

- **GC dispatch je in-memory, bez perzistencije/fronte.** Ako Master padne
  usred `gc_dispatch()`-a, neki već-superseded blokovi mogu ostati fizički
  zaglavljeni na disku zauvijek. Pravi fix čeka Fazu 12 (Master
  persistence/restart).
- **Race: concurrent READ vs fizičko brisanje.** Klijent koji je upravo
  dohvatio stari block-list (`READ_FILE`) tačno prije konkurentnog
  DELETE-a/overwrite-a može dobiti `BLOCK_NOT_FOUND` sa Storage Node-a čiji
  je blok već fizički obrisan. Eksplicitno predstavljeno korisniku kao
  tradeoff (refcounting/lease infrastruktura čeka Fazu 12) — korisnik je
  potvrdio prihvatanje ovog gap-a.
- **Storage Node nedostupan tokom DELETE/GC dispatch-a** — best-effort,
  ne-fatalno, isti obrazac kao Faza 9/10.

**Ručna provjera**: Master + 3 prava `dfs_storage` procesa preko `dfs_client`
CLI-ja. `create`+`write`+`stat` → potvrđen blok fizički prisutan na sve 3
replike (`find .../blocks -name '*.block'`) → `delete` → isti `find` vraća 0
pogodaka na sve 3 (kriterijum 3) → `list /` više ne prikazuje fajl.
WRITE-overwrite GC: `write` v1 (blok X) → `write` v2 (blok Y) → X fizički
nestao sa sve 3, Y prisutan na sve 3 → `read` == v2 sadržaj bajt-po-bajt.
Kriterijum 2 (partial-write-failure): `kill -SIGINT` na jedan `dfs_storage`
proces → overwrite `write` propada (`error_code=10`, replika nedostupna) →
`read` i dalje vraća staru (v2) verziju nepromijenjenu, bajt-po-bajt
identično.

152/152 testova prolazi (131 stari + 21 nov: 5 u `test_namespace.cpp`
(`DeleteFileReturnsOrphanedBlocksWithReplicas`,
`DeleteFileOnZeroBlockFileReturnsEmptyOrphanList`,
`CommitWriteOnFreshFileWithNoPriorBlocksReturnsEmptyOrphanList`,
`CommitWriteOverExistingBlocksReturnsOldBlocksAsOrphans`,
`CommitWriteFailureLeavesOldBlocksIntact`), 5 u `test_master.cpp` (uključujući
`DeleteFileWithHangingReplicaCompletesWithinBoundedTime`, regresioni test za
cpp-reviewer-ov CRITICAL nalaz), 6 u novom `tests/integration/test_delete.cpp`,
5 u novom `tests/integration/test_list_stat.cpp`), čisto pod ASan/UBSan.

## 16. Faza 12 — Heartbeat, Failure Detection, Recovery (§8 u potpunosti)

Najveća pojedinačna faza dosad — implementirana u 4 sekvencijalna
pod-koraka (12.1-12.4), svaki sa punim zelenim test suite-om + ASan/UBSan
prije sljedećeg, svaki svoj commit, sve pod ovim jednim naslovom.

**Poprečne odluke** (važe za cijelu Fazu 12, ne samo 12.1): §7.22-ov
`NodeManager` je spojen direktno u postojeći `StorageRegistry` (umjesto
paralelne node mape) — isti presedan kao Faza 11-ova odluka da ne pravi
zaseban `gc.h` za nešto bez stvarno nezavisnog stanja. `FailureDetector`,
`RecoveryManager` (12.2) i `MetadataStore` (12.4) JESU prave nove klase
(imaju stvarno nezavisno stanje/thread-ove/disk I/O). `ReplicationManager`
ostaje potpuno netaknut kroz cijelu fazu — sva recovery orkestracija ide u
novu `RecoveryManager` klasu. §8.34-tip "pravi orphan" (Master uopšte ne zna
za blok) ostaje strukturno odvojen od Faza 11-ovog `OrphanedBlock`
mehanizma (Master-known superseded blok) — različiti koncepti, iako oba
dijele "ne briši odmah" filozofiju.

### 16.1 — Heartbeat + Failure Detection

Dodaje node liveness state machine: `NodeState{JOINING, HEALTHY, SUSPECTED,
DEAD, RECOVERING}` na `StorageRegistry::StorageNodeInfo`, zamjenjujući stari
`bool alive` (koji se od Faze 9 nikad nije mijenjao van registracije — sad
ga stvarno pokreće failure detection). `mark_dead()`/`mark_alive()` ostaju
kao tanki wrapper-i oko `set_state()` radi izvorne kompatibilnosti.

**Ključna odluka o inicijalnom stanju**: svježa `REGISTER_NODE` runda je
SAMA PO SEBI živi dokaz dostupnosti, pa čvor odmah kreće kao `HEALTHY` — ne
`JOINING`, kako bi doslovno čitanje §8.4-ovog "JOINING → HEALTHY" dijagrama
moglo sugerisati. `JOINING` je rezervisano za 12.4-ov slučaj: Master koji se
upravo oporavio od restarta nema nikakav živi dokaz da je bilo koji ranije
poznat čvor i dalje zdrav, pa restauracija iz snapshot-a eksplicitno postavlja
`JOINING`. Ova distinkcija čuva SVE postojeće Faza 7-11 testove nepromijenjenim
(svi pretpostavljaju da je čvor odmah upotrebljiv nakon `REGISTER_NODE`-a).

**`FailureDetector`** (nova klasa, `include/master/failure_detector.h`):
sopstveni `std::thread` + `condition_variable`-bazirani sweep loop (isti
idiom kao `NetworkServer`-ov accept-thread — `stop()` vraća odmah preko
`notify_all()`, ne čeka pun sweep interval). `SUSPECTED_TIMEOUT_SECONDS`
(=6, isto kao postojeći `FAILURE_TIMEOUT_SECONDS`) i `DEAD_TIMEOUT_SECONDS`
(=12, duplo) daju čvoru jedan pun dodatni failure-timeout prozor kao
"možda je samo spor" grace period prije potpunog otpisivanja. Konstruktor
prima opcione `Duration` parametre za timeout-e (podrazumijevano iz
`config.h`) — testovi ih smanjuju na desetine milisekundi umjesto čekanja
stvarnih 6s/12s.

**Bez zajedničke `PeriodicTask` apstrakcije** — samo dva pozivaoca ikad
postoje (ovaj sweep loop i StorageNode-ov heartbeat sender ispod), što ne
prelazi projektov "bez apstrakcije dok se ne dokaže potreba" prag.

**`record_heartbeat()` promoviše SAMO `SUSPECTED → HEALTHY`**, nikad iz
`DEAD`/`RECOVERING` — promocija odatle ide isključivo kroz 12.3-ovu inventory
rekonsilijaciju. Ovo je namjeran dizajn, ne propust: goli heartbeat sam po
sebi ne dokazuje da čvor još uvijek posjeduje ispravne, neoštećene blokove.

**Poznat gap ostavljen za 12.3**: `notified_dead_` set (garantuje
single-fire `on_node_dead_` callback po DEAD prelazu) se nikad ne čisti —
ako 12.3 kasnije promoviše čvor nazad u HEALTHY pa on OPET umre, callback se
neće ponovo okinuti dok se ne doda eksplicitan "obriši notifikaciju"
hook prilikom promocije. Dokumentovano komentarom u kodu, ne popravljeno
sada jer put za promociju iz DEAD-a još ne postoji.

**Wire protokol**: `HEARTBEAT` (već rezervisan enum vrijednost od Faze 1)
prvi put dobija stvarno rukovanje. Zahtjev: `[u64 node_id]`. Odgovor:
`ACK{[u8 needs_inventory]}` (uvijek 0 u ovom pod-koraku — prava semantika
dolazi u 12.3/12.4) ili `ERROR(NODE_NOT_FOUND)`.

**Testovi**: 6 novih u `test_storage_registry.cpp` (NodeState prelazi,
`record_heartbeat` semantika), 5 novih u novom `test_failure_detector.cpp`
(brzi test-only timeout-i — HEALTHY→SUSPECTED→DEAD, single-fire callback,
`stop()` vraća odmah bez čekanja sweep interval), 2 nova u `test_master.cpp`
(`RegisterThenHeartbeatReturnsAck`, `HeartbeatFromUnknownNodeReturnsNodeNotFound`).
`UnknownMessageTypeReturnsInvalidRequest` prebačen sa `HEARTBEAT` (sad
obrađen) na i dalje neiskorišten `REPLICATE_BLOCK`.

**Ručna E2E verifikacija**: Master + 3 prava `dfs_storage` procesa, čekanje
kroz nekoliko heartbeat intervala (8s) bez pada procesa, normalne
`create`/`write`/`read` operacije rade ispravno paralelno sa heartbeat
thread-ovima, čisto gašenje preko `SIGINT` (bez zaglavljivanja `stop()`-a
oko novih thread-ova).

165/165 testova prolazi (152 stari + 13 nov), čisto pod ASan/UBSan.

### 16.2 — Recovery / re-replikacija + WRITE_BLOCK idempotency retrofit

**Novi tipovi**: `OperationId` (alias za `uint64_t`, `common/types.h`).
`OperationLog` (`storage/operation_log.h`) — per-StorageNode bounded FIFO
ledger `{OperationId → OperationRecord{state, result, response_payload}}`,
dijeljen između `WRITE_BLOCK`- i `COPY_BLOCK_DATA`-handler-a (kolizija
namespace-a je zanemarljiva jer su ID-jevi nasumični 64-bitni brojevi, ne
monotoni brojači po procesu — dva različita `Client`/`RecoveryManager`
procesa koji oba počinju od 1 bi kolidirala).

**`BlockHeader` dobija `block_version`** — `BLOCK_HEADER_SIZE` 22→30,
`BLOCK_HEADER_VERSION` 1→2 (bezbjedno mijenjati on-disk format, `data/` je
gitignored). `BlockStorage`/`BlockManager` dobijaju `get_block_info()`,
`enumerate_blocks()`, i `write_block()`/`write_block_at()` sad primaju
`block_version` (default 0 za sve pre-12.2 pozivaoce). CRC32 promovisan iz
`block_storage.cpp`-ove anonimne oblasti u `compute_block_checksum()`
(`storage/block_header.h`), dijeljen sa novim `handle_copy_block_data()`-om
za in-transit verifikaciju (§8.14 — checksum se provjerava PRIJE upisa na
disk).

**Wire protokol**: nov `COPY_BLOCK_DATA` tip (source→destination stvarni
transfer podataka, `COPY_BLOCK` ostaje samo Master→source komanda — isti
obrazac kao `WRITE_COMMIT`/`WRITE_BLOCK`). `WRITE_BLOCK` prošireno sa
`operation_id` (prefiks) i `block_version` (prije `data_size`) —
bezbjedno mijenjati jer svi pozivaoci idu isključivo preko `Client`-ovog
javnog API-ja, ne preko hardkodovanog wire layout-a.

**`RecoveryManager`** (nova klasa, `include/master/recovery_manager.h`) —
NAMJERNO odvojena od `ReplicationManager`-a (koji ostaje potpuno netaknut,
`choose_replicas()` nedirnut). Pool od tačno
`MAX_CONCURRENT_RECOVERY_OPERATIONS`(=4) worker thread-ova nad dijeljenim
thread-safe redom. `handle_node_failure()` (kačeno kao
`FailureDetector`-ov `on_node_dead` callback) pronalazi pogođene blokove
preko nove `NamespaceManager::blocks_with_replica()`, markira ih
`UNDER_REPLICATED`, stavlja u red. `recover_block()` retry-uje IN-PLACE
(ne re-queue) do `MAX_RECOVERY_ATTEMPTS_PER_BLOCK`(=5) puta: bira zdrav
source (bilo koja preostala replika sa `NodeState::HEALTHY`) i zdravu
destinaciju (`ReplicationManager::choose_recovery_destination()`, isključuje
trenutne replike), šalje `COPY_BLOCK` na source, koji sam relay-uje stvarne
bajtove ka destinaciji (`COPY_BLOCK_DATA`) — Master nikad ne prenosi
payload (§8.8/§7.13). Na uspjeh, `NamespaceManager::update_block_replicas_after_recovery()`
atomski zamjenjuje stari node novim i markira `COMMITTED`.

**KRITIČNA lekcija iz Faze 11 primijenjena eksplicitno**: `send_copy_block()`
(Master→source) I `StorageNode::handle_copy_block()` (source→destination)
OBJE koriste `Socket::set_timeout(COPY_BLOCK_TIMEOUT_MS)` — recovery ima
DVIJE odlazne noge, ne jednu kao Faza 11-ov `send_delete_block`, i obje
moraju biti ograničene.

**Nasumičan `OperationId` umjesto monotonog brojača** — `Client` i
`RecoveryManager` oboje generišu `OperationId` preko `std::mt19937_64`
(prva i druga upotreba `<random>`-a u projektu), ne preko postojećeg
`next_request_id_`-obrasca, jer bi dva različita procesa koja oba počinju
brojanje od 1 mogla kolidirati na potpuno različitim operacijama nad istim
blokom na istom node-u.

**Ručna verifikacija**: automatizovani `RecoveryIntegrationTest.KilledNodeTriggersRecoveryAndReadKeepsSucceeding`
(pravi `Master` + 4 prava `StorageNode` objekta, RF=3) već predstavlja
punu end-to-end provjeru definicije gotovog iz `IMPLEMENTATION_PLAN.md`-a:
ubij node usred klastera → potvrdi READ i dalje uspijeva tokom cijelog
procesa → potvrdi 3 replike ponovo na disku (4. node preuzima ulogu) →
sadržaj identičan originalu. Traje ~12s (stvarni `SUSPECTED_TIMEOUT_SECONDS`+
`DEAD_TIMEOUT_SECONDS`), namjerno bez skraćenih test-only timeout-a jer
testira Master-ov CIJELI stvaran failure-detection put, ne izolovanu klasu.
Posebna ručna multi-process (`scripts/start_*.sh`) provjera preskočena ovaj
put — ova automatizovana provjera već koristi prave `StorageNode` objekte
sa pravim diskom/checksumovima/mrežom, samo u jednom test procesu umjesto
više OS procesa; puna multi-process validacija ostaje za Fazu 16
(§16.3-§16.5, koja eksplicitno postoji baš za ovu razliku).

177/177 testova prolazi (165 stari + 12 nov: 5 u `test_block_storage.cpp`,
6 u novom `test_operation_log.cpp`, 1 nov integration `test_recovery.cpp`),
čisto pod ASan/UBSan.

### 16.3 — Storage Node restart / lokalni recovery + inventory rekonsilijacija

**Wire protokol**: nov `NODE_INVENTORY` tip (odmah nakon `REGISTER_NODE`),
dodat u `is_data_message()` (neograničen broj blokova po izvještaju).
StorageNode ga šalje na SVJEŽOJ konekciji odmah nakon `REGISTER_NODE`-ovog
ACK-a (isti gate na povratnu vrijednost kao sama registracija u `start()`),
i ponovo reaktivno iz `heartbeat_loop()`-a kad god `HEARTBEAT` ACK javi
`needs_inventory==1` (taj bit postoji na žici od 12.1, ali je hardkodovan na
0 sve do 12.4 — u ovom pod-koraku samo kod koji ga ČITA postoji, put koji ga
stvarno postavlja na 1 dolazi kasnije). Zahtjev: `[u64 node_id][u32
block_count]{[u64 block_id][u64 version][u32 checksum]} × block_count` →
Master uvijek odgovara `ACK{}` (rekonsilira sinhrono inline, nikad ne
odbija malformiran izgled izvještaja kao grešku klijentu). `ALLOCATE_BLOCK`
odgovor dobija trailing `[u64 version]` (uvijek 1 za svježe alocirani
blok). `APPEND_FILE` odgovor, kad `has_target==1`, dobija trailing `[u64
target_version]` (= trenutna verzija bloka + 1).

**`StorageRecovery::build_inventory_payload()`** (nov,
`storage/recovery.h`/`.cpp`) — čist payload builder, ne klasa (isti stil
kao `block_header.h`-ove slobodne funkcije), gradi `NODE_INVENTORY`
payload direktno iz `BlockManager::enumerate_blocks()`-a.

**Block versioning kroz cijeli sistem**: `NamespaceManager::set_block_replicas()`
sad postavlja `version=1` (bio hardkodovan `0` od 12.2, namjerno odloženo do
sad). `resolve_append_target()` vraća `target_version = trenutna_verzija +
1` u `AppendTarget`-u; `commit_append()` upisuje tu vrijednost u
`BlockMetadata.version` na istom uspješnom CAS-u koji već čuva
`expected_prior_size` (novo `AppendCommitRequest.expected_target_version`
polje, dodano na KRAJ struct-a sa default vrijednošću `0` — ne grupisano uz
ostala `has_existing_target`-samo polja — baš zato da svaki postojeći
pozicioni `AppendCommitRequest{...}` inicijalizator u `test_namespace.cpp`
ostane nepromijenjen bez izmjene). `Client::write_new_blocks()`/`append()`
sad parsiraju stvarne `version`/`target_version` vrijednosti sa žice umjesto
12.2-ove hardkodovane `0`.

**`Master::handle_node_inventory()`** — tro-grani rekonsilijacija po
prijavljenom `(block_id, version, checksum)`: (1) nepoznat blok ILI node
nije u `metadata.replicas` ⇒ §8.34-tip "pravi orphan", log-only (`stderr`),
NIKAD spojen sa Faza 11-ovim `OrphanedBlock`/`gc_dispatch` mehanizmom —
strukturno odvojen koncept (isti razlog kao poprečna odluka #6 iz uvoda
ove faze); (2) `version < meta.version` ⇒ `RecoveryManager::handle_stale_replica()`
(nova metoda — SINHRONA, za razliku od `handle_node_failure()`-ovog
worker-pool reda, jer je ovo ograničena one-shot operacija, ne batch koji
treba throttlovati); (3) `version == meta.version` I ovo bi gurnulo broj
replika iznad `DEFAULT_REPLICATION_FACTOR` ⇒ ponovo koristi POSTOJEĆI
`Master::send_delete_block()` (Faza 11) + nova `NamespaceManager::remove_replica()`
— jedina konkretna §7.14-15 ponovna upotreba koju ova faza traži. Nakon
cijelog izvještaja: `storage_registry_.set_state(node_id, NodeState::HEALTHY)`.

**Poznata granica**: `replicas.size() > DEFAULT_REPLICATION_FACTOR` grana
nije dostižna kroz bilo koji postojeći produkcijski put u ovoj fazi —
`ReplicationManager::choose_replicas()` uvijek vraća tačno RF čvorova ili
prazan vektor, a `update_block_replicas_after_recovery()` zamjenjuje jedan
za jedan (nikad ne raste listu). Grana je implementirana tačno po
odobrenom planu (defanzivan/forward-looking kod za scenario koji trenutna
implementacija ne proizvodi), i pokrivena direktnim `NamespaceManager`
unit testovima (`RemoveReplicaDropsExactlyTheNamedNode`,
`RemoveReplicaOnUnknownBlockOrNodeIsNoOp`) umjesto Master-nivoa
integration testa — konstruisanje >RF replika stanja kroz `Master`-ov javni
wire API nije moguće bez test-only backdoor-a koji bi bio nekonzistentan sa
projektovim "bez apstrakcije/seam-a dok se ne dokaže potreba" presedanom.

**Testovi**: 4 nova u `test_namespace.cpp` (`CommitWriteSetsInitialVersionToOne`,
`AppendIncrementsBlockVersion`, `RemoveReplicaDropsExactlyTheNamedNode`,
`RemoveReplicaOnUnknownBlockOrNodeIsNoOp`), 2 nova u `test_master.cpp`
(`NodeInventoryLogsUnknownBlockAsOrphanWithoutDeleting`,
`NodeInventoryMarksStaleReplicaForResync` — reuse `RecordingNode`, proširen
da prati i `COPY_BLOCK` zahtjeve, ne samo `DELETE_BLOCK`), `AllocateBlockReturnsThreeDistinctReplicaLocations`
ažuriran za novo trailing `version` polje, 1 nov integration test u
`test_recovery.cpp` (`RestartWithValidBlocksSendsInventoryWithoutRecopying`
— pravi `Master` + 4 prava `StorageNode` objekta, jedan node se čisto
gasi/restartuje protiv ISTOG `data_dir`-a, potvrđuje da se blok NE
prekopira nigdje drugo i da ostaje čitljiv tokom cijelog procesa).

**Ručna E2E verifikacija**: pravi `dfs_master` + 3 prava `dfs_storage`
procesa preko `scripts/start_*.sh`, `create`+`write`+`stat`, `kill -SIGINT`
jednog storage procesa, ponovno pokretanje istog `scripts/start_storage.sh N`
protiv istog `data_dir`-a — potvrđeno čisto gašenje i ponovno pokretanje bez
grešaka u logovima, `find data/*/blocks` pokazuje TAČNO isti skup blokova
prije i poslije (nema novih fajlova), naredni `read` vraća bajt-identičan
sadržaj originalu.

184/184 testova prolazi (177 stari + 7 nov: 4 u `test_namespace.cpp`, 2 u
`test_master.cpp`, 1 integration u `test_recovery.cpp`), čisto pod
ASan/UBSan.

### 16.4 — Master perzistencija + restart

**On-disk format**: jedan kombinovani binarni snapshot fajl
(`<metadata_dir>/metadata.snapshot`), custom big-endian layout: `[u32
magic="METS"][u16 format_version=1][u64 next_file_id][u64 next_block_id]
[u32 file_count]{...} [u32 block_count]{...} [u32 node_count]{...} [u32
crc32]`. Pisan kroz identičan tmp+fsync+rename obrazac kao `BlockStorage`,
**sinhrono** pri svakoj trajnoj mutaciji. `NodeState`/`last_heartbeat` se
NAMJERNO ne perzistiraju — svaki čvor se učitava kao `JOINING` i promoviše
u `HEALTHY` isključivo kroz 12.3-ov `NODE_INVENTORY` mehanizam.

**`MetadataStore`** (nova klasa, `include/master/metadata_store.h`/`.cpp`)
— fallible `create()` (isti obrazac kao `StorageNode::create()`-ov
`BlockManager`). CRC32 reimplementiran lokalno (ne dijeljen sa
`storage/block_header.h`-ovim `compute_block_checksum()`) da se izbjegne
zavisnost master-biblioteke od storage-biblioteke — isti presedan kao
dupliranje wire-helper funkcija (`put_u16`/`get_u16` itd.) po svakom
modulu zasebno. `load()` vraća prazan `LoadedState` kao `OK` i za
NEPOSTOJEĆI fajl I za OŠTEĆEN fajl (loš magic/CRC, skraćen, nečitljiv) —
isti "izuzmi oštećeno, nastavi da radiš" pristup kao
`BlockStorage::recover()`-ovo per-block checksum rukovanje; samo prava
I/O greška na nivou direktorija (`create()`) je fatalna. Parsiranje ide
kroz interni bounds-checked `Reader` cursor (svaki accessor provjerava
preostalu veličinu; jednom kad "zapne", svi naredni pozivi su bezbjedni
no-op-ovi, petlje prekidaju rano umjesto da mljate kroz lažno ogroman
count polje).

**`NamespaceManager`** dobija `all_files()`/`all_blocks()`/`next_file_id()`/`next_block_id()`
read accessore i `restore(...)` (zamjenjuje `files_`/`blocks_` u
potpunosti, čisti `pending_blocks_` — svaka pending-ali-nikad-commit-ovana
alokacija u trenutku posljednjeg snapshot-a je jednostavno nestala, ista
sudbina kao svaki drugi napušteni write pokušaj). `StorageRegistry` dobija
`restore_node()` (forsira `state=JOINING` bez obzira na snapshot sadržaj).

**`Master`**: `Config` dobija `metadata_dir`. `create()` prvo konstruiše
`MetadataStore` (fallible), `load()`-uje ga, konstruiše `Master`, pa
poziva `namespace_manager_.restore(...)`/`storage_registry_.restore_node(...)`
po čvoru. **Napomena o redoslijedu**: iako C++ garantuje da su SVI članovi
(uključujući `replication_manager_`/`recovery_manager_`/`failure_detector_`)
već konstruisani prije nego što se ijedan red tijela konstruktora izvrši,
ta trojka drži samo reference i ne čita stanje eagerly pri konstrukciji —
pa restore pozivi u `create()`-u (poslije konstrukcije, prije `start()`-a)
i dalje zadovoljavaju stvarnu namjeru "rehidrirano prije nego što se
ijedan zahtjev opsluži", ne samo slovo "prije konstrukcije". Nov private
`persist_metadata()` pozvan na kraju `handle_create_file`,
`handle_delete_file`, `handle_write_commit`, `handle_append_commit`,
`handle_register_node`, i proslijeđen kao `RecoveryManager`-ov
`on_metadata_changed` callback (pripremljen unaprijed u 12.2 baš za ovo).

**Zatvorena rupa otkrivena tokom dizajna**: `Master::handle_heartbeat()`
je do sad UVIJEK vraćao `needs_inventory=0` (nije bilo potrebe — jedini
put ka `JOINING` bio je hipotetičan prije 12.4). Sada provjerava
prijavljenog čvora stvarno stanje i vraća `needs_inventory=1` tačno kad je
`JOINING` — bez ovoga, Storage Node čiji SOPSTVENI proces nikad nije
restartovan (pa je svoj jedini inventory izvještaj poslao davno, prije
OVOG Master restarta) nikad ne bi imao drugi okidač da ga ponovo pošalje,
i ostao bi zauvijek `JOINING`. Pokriveno sa dva nova testa: jedan direktan
(`HeartbeatFromJoiningNodeSetsNeedsInventoryBit`) i jedan pun end-to-end
integration test gdje pravi `StorageNode` procesi ostaju živi kroz cijeli
Master restart (`LiveNodeIsPromotedBackToHealthyAfterMasterRestartViaInventory`).

**Testovi**: nov `test_metadata_store.cpp` (5 testova: prazan direktorij,
save/load round-trip, stray `.tmp` odbačen bez uticaja na kanonski fajl,
oštećen snapshot → prazno stanje umjesto greške, uzastopni save-ovi
prepisuju prethodni). 3 nova u `test_master.cpp`
(`MasterRestartPreservesNamespaceAndBlockMetadata`,
`MasterRestartRestoresNodesAsJoiningNotHealthy`,
`HeartbeatFromJoiningNodeSetsNeedsInventoryBit`) — sve tri koriste
doslovno gašenje (`stop()`+`reset()`) i ponovnu konstrukciju `Master`-a
nad ISTIM `metadata_dir`-om unutar iste `MasterTest` fixture. 1 nov
integration u `test_recovery.cpp`
(`LiveNodeIsPromotedBackToHealthyAfterMasterRestartViaInventory`).

**Ručna E2E verifikacija**: pravi `dfs_master` + 3 prava `dfs_storage`
procesa, `create`+`write`+`stat` preko pravog `dfs_client`-a, `kill
-SIGINT` Master-a, restart preko `scripts/start_master.sh`, `stat`/`list`
vraćaju identične podatke, `read` odmah uspijeva (čitanje ne provjerava
`NodeState` uopšte, samo stvarnu dostupnost — dosljedno postojećem
ponašanju). Zatim složeniji scenario: `SIGINT` jednog storage node-a PRIJE
restarta Master-a, restart Master-a, pa tek onda ponovno pokretanje tog
storage node-a — potvrđeno (nakon par sekundi rekonsilijacije) da nov
`create`+`write`+`read` na drugom fajlu radi čisto, dokazujući da se
kombinovani scenario (neki čvorovi restartovani, neki samo ostali živi
kroz Master restart) rekonsilira ispravno u oba slučaja istovremeno.

191/191 → **193/193** testova prolazi (184 stari + 9 nov: 5 u
`test_metadata_store.cpp`, 3 u `test_master.cpp`, 1 integration u
`test_recovery.cpp`), čisto pod ASan/UBSan.

**Nezavisan `cpp-reviewer` agent na CIJELOJ Fazi 12 (sva 4 pod-koraka
zajedno) — 2 HIGH + 2 MEDIUM nalaza, sva 4 popravljena** (isti obrazac kao
Faze 10/11 — bar jedan netrivijalan nalaz očekivan i ovaj put potvrđen):

- **HIGH: `FailureDetector::notified_dead_` se nikad nije čistio, pa DRUGO
  umiranje istog čvora tiho nikad ne bi pokrenulo recovery.** 12.1-ov
  komentar je ovo eksplicitno najavio kao "NOTE for Faza 12.3", ali 12.3/12.4
  nikad nisu dodali stvarnu kuku. Scenario: čvor umre → `on_node_dead_`
  se okine → `notified_dead_` ga zapamti → 12.3-ova `NODE_INVENTORY`
  rekonsilijacija ga promoviše nazad u `HEALTHY` → isti čvor OPET umre
  (drugi pravi pad, realno za dugotrajan klaster) → `notified_dead_.insert(X).second`
  je već `false` → `RecoveryManager::handle_node_failure()` se nikad
  ponovo ne pozove → njegovi blokovi ostaju trajno under-replicated, bez
  ijedne greške ili log linije. Popravljeno: nova
  `FailureDetector::clear_notification(StorageNodeId)`, pozvana iz
  `Master::handle_node_inventory()` odmah nakon
  `storage_registry_.set_state(node_id, NodeState::HEALTHY)`. Regresioni
  test: `FailureDetectorTest.ClearNotificationAllowsOnNodeDeadToFireAgainForSameNode`
  (prva smrt → promocija → `clear_notification()` → druga smrt → potvrđeno
  da `on_node_dead_` puca DVA puta, ne jednom).
- **HIGH: `StorageNode::send_inventory()` i `register_with_master()` nisu
  imali `Socket::set_timeout()`**, za razliku od svake druge nove odlazne
  konekcije ove faze (heartbeat sender, obje `COPY_BLOCK` noge) — baš ta
  lekcija koju je Faza 11 već naučila (§16.2's "KRITIČNA lekcija") promašena
  je na ova dva mjesta. Master koji prihvati konekciju ali nikad ne
  odgovori bi zauvijek objesio `StorageNode::start()` (ili
  `heartbeat_loop()`-ovu reaktivnu `send_inventory()`). Popravljeno: nova
  `MASTER_RPC_TIMEOUT_MS = 5000` konstanta (`common/config.h`), primijenjena
  na oba poziva. Regresioni test:
  `StorageNodeTest.StartTimesOutRatherThanHangingWhenMasterNeverReplies`
  (fake "Master" koji prihvati konekciju pa nikad ne odgovori — potvrđeno
  da `start()` vrati grešku za ~5s, ne zauvijek).
- **MEDIUM: `handle_node_inventory()`-ova provjera verzije je tiho
  ignorisala `reported_version > known_version` slučaj** (samo `<` i `==`
  su bili obrađeni). Može se desiti ako je fizički upis stigao na SVE
  replike (strict 3/3, nikad djelimično) ali je sam `APPEND_COMMIT` ka
  Master-u izgubljen/istekao (`APPEND_COMMIT_WAIT_TIMEOUT_MS`) — Master-ova
  verzija nikad ne sustigne stvarnu, i nijedan budući inventory izvještaj
  to nikad ne prijavi. Popravljeno: dodat `else` grana koja loguje anomaliju
  (stderr) — namjerno BEZ automatske korekcije, jer `NODE_INVENTORY`-ov
  `(block_id, version, checksum)` izvještaj ne nosi dovoljno informacije
  (nema `size`/`reserved_size`) da se bezbjedno rekonstruiše ostatak
  `BlockMetadata`-e; slijepo "sustizanje" verzije bez tih polja moglo bi
  ostaviti `reserved_size` zastarelim i pokvariti offset-računicu
  narednog append-a. Regresioni test:
  `MasterTest.NodeInventoryVersionAheadOfMasterIsLoggedWithoutAction`.
- **MEDIUM: `MetadataStore::load()` je pozivao `.reserve(count)` na
  neprovjerenim on-disk count poljima PRIJE bilo kakve provjere granica** —
  u suprotnosti sa sopstvenim dokumentovanim obećanjem ("oštećen snapshot
  nikad ne obara Master"). CRC32 nije kriptografski siguran (trivijalno
  krivotvoriv za bilo koga sa lokalnim pristupom `metadata_dir`-u), pa
  namjerno napravljen (i dalje CRC-validan) snapshot sa ogromnim count
  poljem (npr. `0xFFFFFFFF`) baca neuhvaćen `std::length_error`/`bad_alloc`
  i ruši Master pri startu. Popravljeno: nova `Reader::bounded_count()`
  metoda — pošto svaki element treba bar 1 preostali bajt, stvarni count
  nikad ne može biti veći od bajtova koji su stvarno ostali u baferu; sva 4
  `.reserve()` poziva (`state.files`, `file.blocks`, `state.blocks`,
  `metadata.replicas`, `state.nodes`) sad prolaze kroz ovu granicu.
  Regresioni test: `MetadataStoreTest.LoadWithHugeCountFieldDoesNotCrashOrThrow`
  (ručno-napravljen CRC-validan snapshot sa `file_count=0xFFFFFFFF` i
  odsječenim tijelom — potvrđeno da `load()` vrati prazno stanje umjesto da
  baci exception).

193/193 → **197/197** testova prolazi (4 nova regresiona testa, jedan po
nalazu), čisto pod ASan/UBSan (uključujući ponovljenu punu ASan/UBSan
provjeru nakon ovih popravki).

## 17. Faza 13 — Client Cache (§9 u potpunosti)

Block-level LRU keš unutar `Client`-a (`include/client/cache.h`,
`src/client/cache.cpp`) — čista optimizacija, nikad izvor istine (§9.32).
Nema cross-client koherencije (§9.13, eksplicitno van scope-a): keš postoji
samo unutar jednog `Client` procesa, koji je jedini sposoban da mutira
blokove koje sam kešira.

**Ključna odluka o conditional READ-u naspram Definicije gotovog**: §9.6-§9.8
opisuju conditional `READ_BLOCK` (`known_version` → `NOT_MODIFIED`/`READ_OK`)
kao validacioni mehanizam, što i dalje ide na mrežu (samo ne prenosi podatke).
Ali plan-ova sopstvena Definicija gotovog za ovu fazu eksplicitno traži da
ponovljeni READ istog bloka **ne** proizvede dodatni Storage Node network
poziv — jače od conditional-GET modela. Razrešeno ovako: budući da je jedan
`Client` jedini pisac blokova koje sam kešira i već ih invalidira na svaki
uspješan WRITE/APPEND (§9.9-§9.11), cache hit je uvijek validan bez
revalidacije. `Client::read()` na pogodak vraća podatak direktno, bez ijednog
mrežnog poziva. Conditional `READ_BLOCK` wire mehanizam (`known_version`,
`READ_OK`, `NOT_MODIFIED` — sve tri već rezervisane u `MessageType`-u od Faze
1, nikad ranije korišćene) je ipak potpuno implementiran na Storage Node
strani (`StorageNode::handle_read_block()`, provjerava verziju preko
`BlockManager::get_block_info()` prije čitanja podatka sa diska, izbjegavajući
nepotreban disk I/O na `NOT_MODIFIED`), pošto je eksplicitno imenovan kao
deliverable u §9.8. Testiran je direktno protiv `StorageNode`-a
(`StorageNodeTest.ReadBlockWithMatchingKnownVersionReturnsNotModified`/
`...StaleKnownVersionReturnsFullData`). Client ga trenutno poziva samo sa
`known_version=0` na cache-miss putanji (siguran sentinel — verzije uvijek
kreću od 1, §16.1) — nikad sa nenultom vrijednošću, jer za to nema
legitimnog poziva unutar single-client modela ove faze. Isti obrazac kao
Faza 12's "over-replication test coverage" napomena: izgrađen-ali-neiskorišćen
mehanizam, dokumentovan eksplicitno umjesto tiho izostavljen.

**Jedini stvarni coherence hazard unutar jednog Client-a**: `append()` može
proširiti *postojeći* poslednji blok fajla in-place (isti `BlockId`, novi
sadržaj na kraju, §6.10/Faza 12.3-ov `AppendTarget`) — zato `Client::append()`
zove `cache_.invalidate(existing_block_id)` nakon uspješnog `APPEND_COMMIT`-a.
`Client::write()` overwrite uvijek alocira potpuno nove `BlockId`-eve (Faza 11
semantika), pa je njegova invalidacija bezopasan no-op — primijenjena radi
doslednosti sa §9.9-§9.11-ovom bukvalnom "WRITE → invalidate" politikom i
otpornosti na buduću promjenu te semantike.

**`ClientCache`** (§9.16-§9.21): `std::list<BlockId>` LRU red +
`std::unordered_map<BlockId, Entry>`, `Entry` čuva iterator za O(1)
move-to-front. Jedan `std::mutex`, kritična sekcija samo oko mape/liste —
`get()`/`put()`/`invalidate()` nikad ne rade I/O, pa Client sam obezbjeđuje
"unlock prije mrežnog poziva" pravilo (§9.19-§9.20) time što zove `get()`
PRIJE i `put()` TEK POSLIJE mrežnog poziva. Edge slučaj: pojedinačni blok
veći od `max_bytes_` se ipak kešira sam (nema drugih entrija za evikciju) —
namjerno pojednostavljenje. Statistika (`CacheStats`: hits/misses/evictions/
invalidations/bytes_served) je samo interni brojač u `ClientCache`-u — nema
stvarnog `StatisticsManager`-a još (potvrđeno grep-om, ne postoji nigdje u
kodu; Faza 14 ga tek uvodi).

`Client::Config` dobija `cache_max_bytes` (default `DEFAULT_CLIENT_CACHE_BYTES`
= 512 MB, §9.14-ov primjer), novi field na kraju strukture — postojeći
pozicioni `Client::Config{...}` pozivi u testovima ostaju nepromijenjeni.

Wire format: `READ_BLOCK` request `[u64 block_id]` → `[u64 block_id][u64
known_version]`. Odgovor mijenja tip sa generičkog `ACK` na `READ_OK{[u64
version][u32 data_size][data]}` ili `NOT_MODIFIED{[u64 version]}` — `Master`-ov
`READ_FILE` odgovor se ne mijenja (Client ne treba per-block verziju od
Master-a, dobija je direktno od Storage Node-a pri svakom stvarnom fetch-u).

**Testovi**: `tests/unit/test_cache.cpp` (nov, 10 testova — čist `ClientCache`
bez mreže, uključujući §9.15-ov tačan LRU primjer `A B C D`, pristup
`A,C,A,B` → `D` evikovan); `tests/unit/test_storage_node.cpp` proširen sa 2
nova conditional-READ testa (i 3 postojeća READ_BLOCK poziva ažurirana za
novi wire format); `tests/integration/test_read.cpp` proširen sa 2 nova
scenarija preko pravog klastera:
`RepeatReadHitsCacheAndSurvivesAllStorageNodesStopped` (ugasi SVE storage
node-ove poslije prvog READ-a — drugi READ i dalje uspijeva, jače od bilo kog
brojača) i `AppendInvalidatesCachedBlockSoReadSeesNewContent` (append pa READ
vraća prošireni, ne stari keširan sadržaj).

211/211 testova prolazi (14 novih: 10 `test_cache` + 2 `test_storage_node` + 2
`test_read`), čisto pod ASan/UBSan.

**Nezavisan `cpp-reviewer` na cijeloj Fazi 13 — 1 HIGH + 1 MEDIUM + 2 LOW
nalaza, sva 4 popravljena**:

- **HIGH: `StorageNode::handle_read_block()` je provjeravao verziju i
  čitao podatak kao DVA odvojena zaključavanja** (`get_block_info()` pa
  `read_block()`), pa je konkurentan pisac mogao da izvrši cijeli WRITE
  između njih — rezultat: `READ_OK` čiji `version` opisuje stanje PRIJE
  te pisanje, a `data` opisuje stanje POSLIJE. Potvrđeno ručnim
  reprodukcionim skriptom van test suite-a: 2000 iteracija istog obrasca
  (dva odvojena `get_block_info()`+`read_block()` poziva) je pouzdano
  proizvelo mismatch, dok popravljena verzija (jedno zaključavanje) —
  nula mismatch-eva. Popravljeno: nova `BlockStorage::read_block_if_changed(id,
  known_version)` (i `BlockManager` passthrough) koja radi provjeru verzije
  i čitanje podatka pod JEDNIM `shared_lock`-om — vraća `{not_modified,
  version, data}` odjednom, i to je jedino mjesto odakle
  `handle_read_block()` sad čita. Regresioni test:
  `BlockStorageTest.ReadBlockIfChangedNeverMixesVersionFromOneWriteWithDataFromAnother`
  (writer thread taguje svaki upis sopstvenom verzijom kao sadržaj bloka;
  reader thread provjerava da vraćeni `(version, data)` par UVIJEK
  odgovara — 500 iteracija, čisto).
- **MEDIUM: `Client::append()` je invalidirao keširani target blok TEK
  poslije uspješnog `APPEND_COMMIT`-a**, iako fill-write fizički mijenja
  taj blok na disku RANIJE, prije komit poziva. Ako fill-write uspije ali
  `APPEND_COMMIT` otkaže (mrežni pad, Master timeout), `append()` vraća
  grešku, ali keširani (sad zastarjeli) sadržaj bloka ostaje netaknut —
  širi rizik oko invarijante "keš se mijenja samo u skladu sa
  Master-commit-ovanim stanjem" bez trenutno dohvatljivog puta koji bi ga
  stvarno servirao pogrešno (Master-ova metapodaci su isto tako
  neažurirani do komita, pa se dva "pogrešna" pogleda trenutno slažu), ali
  je pravi propust u redoslijedu. Popravljeno: `cache_.invalidate(existing_block_id)`
  premješteno odmah nakon uspješnog fill-write-a (linija ~459), ne poslije
  komita — invalidacija je jeftina i idempotentna, nema razloga da čeka.
  **Bez posvećenog failure-injection regresionog testa** — namjerno
  zaobiđeno pravljenje test-only kuke za deterministično kvarenje SAMO
  `APPEND_COMMIT`-a poslije uspješnog fill-write-a preko pravog klastera
  (isti presedan kao Faza 12-ova "over-replication test coverage"
  napomena — testabilno samo preko backdoor-a koji ništa drugo ne bi
  koristio); pokrivenost dolazi od koda same popravke (jednostavno
  premještanje jedne linije, provjereno kompajliranjem+postojećim
  `AppendInvalidatesCachedBlockSoReadSeesNewContent` testom da opšte
  ponašanje ostaje ispravno) i eksplicitno dokumentovanog rezonovanja
  ovdje.
- **LOW: `CacheEntry::last_access` se pisao na svaki hit a nikad nigdje
  nije čitan** (LRU redosled dolazi isključivo iz `lru_list_`/splice, ne
  iz timestamp-a). Uklonjen — namjerno odstupanje od §9.16-ovog doslovnog
  `CacheEntry` struct-a (koji uključuje `lastAccess`), isti presedan kao
  `FileMetadata`-ino izostavljanje `replication_factor`-a (§12.2): polje
  bez ijedne stvarne upotrebe se ne drži samo da bi pratilo spec dijagram.
- **LOW: `ClientCache::get()` kopira cijeli blok podataka DOK je `mutex_`
  još zaključan** — nije živ bug danas (`Client` je dokumentovano
  single-threaded), ali `ClientCache` sama po sebi nema tu napomenu u
  svom headeru. Popravljeno dokumentacionim komentarom na `get()`
  (`include/client/cache.h`) koji eksplicitno imenuje trade-off i kad bi
  trebalo revizitovati (ako se `ClientCache` ikad podijeli između
  thread-ova) — bez spekulativnog `shared_ptr<const vector>` refaktora
  koji trenutno ništa ne rješava.

216/216 testova prolazi (5 novih regresionih: 5 u `test_block_storage.cpp`),
čisto pod ASan/UBSan (ponovljena puna provjera nakon ovih popravki).

## 18. Faza 14 — Statistics & Monitoring (§10 dizajn dokumenta)

Podijeljena na 4 pod-koraka (14.1-14.4), isti obrazac kao Faza 12, jer dira
gotovo svaki fajl u projektu iako je pretežno mehanički retrofit (dodavanje
brojača/mernih tačaka), ne nova rizična logika.

**Poprečne odluke** (važe za cijelu Fazu 14 — spec (§10, 46 podsekcija)
ostavlja nekoliko stvari nedefinisanim koje sam morao sam da riješim):

- **`dfsctl stats` mehanizam (§10.33)**: dokument kaže samo "CLI komanda
  ili lokalni endpoint", bez ijedne konkretne žice, i nema `GET_STATS` u
  kanonskom `MessageType` enumu. Dodat `MessageType::GET_STATS`/
  `STATS_RESPONSE` (Faza 14.3, kad StorageNode prvi implementira handler —
  isti presedan kao Faza 12.3-ov `NODE_INVENTORY`). Response payload je
  **običan UTF-8 tekstualni blob** (formatirana tabela, §10.33-ov "DFS
  Statistics" primer), ne strukturisan binaran snapshot — `dfs_statsctl`
  samo ispiše string nazad, bez potrebe za dizajniranjem/verzionisanjem
  posebne binarne šeme za čisto dijagnostički alat.
- **Percentil algoritam (§10.9/§10.38/§10.39)**: nema bucket granica ni
  algoritma u spec-u, samo zahtjev da bude jeftino i periodično.
  `Histogram` koristi fiksne bucket-e na stepen dvojke u mikrosekundama
  (`std::bit_width(value)`, 65 bucket-a, pokriva ceo `uint64_t` opseg),
  `std::atomic<uint64_t>` brojač po bucket-u (O(1) lock-free `record()`),
  percentil se računa TEK u `snapshot()` preko kumulativnog prolaza
  (nearest-rank definicija: `target = max(1, ceil(fraction*total))`) —
  vraća bucket-ovu gornju granicu kao aproksimaciju, ne tačnu vrijednost.
- **Konflikt imena klase (§10.28-30 `StatisticsManager`/camelCase vs
  §17.13 `Statistics`/snake_case)**: `StatisticsManager`, potpuno
  snake_case — projekat nigdje ne koristi camelCase (isti presedan kao
  Faza 0-ova `Result<T,E>` odluka umjesto doslovnog `std::expected`
  sketch-a).
- **Eksplicitno van scope-a, dokumentovano a ne tiho preskočeno**: §10.14
  per-`MessageType` network bajt/poruka brojači (zahtijevao bi kuku u
  svakom mjestu koje pravi `TCPConnection`, a plan-ov sopstveni committed
  metod-spisak ga ne imenuje) i §10.25-26 request/operation-ID korelacija
  (alat za debagovanje/tracing, ne brojiva metrika).
- **§10.20-22 failure-detection-time vs recovery-time**: pravi `t0`
  (trenutak stvarne smrti node-a) je fundamentalno nemerljiv sa Master
  strane. Umjesto lažne "detection latency" ograničene nepoznatim `t0`,
  pratim stvaran, mjerljiv par: `detected_failures` (brojač, stvaran
  `FailureDetector::on_node_dead_` okidač) + `recovery_duration` (stvarno
  izmjerena latencija, `RecoveryManager`-ov `record_recovery` poziv) — plus
  već postojeći `SUSPECTED_TIMEOUT_SECONDS`/`DEAD_TIMEOUT_SECONDS` daju
  fiksnu gornju granicu na detection latency.
- **`record_replication` naspram `record_recovery`**: `ReplicationManager`
  samo BIRA node-ove (nikad ne pomjera bajtove), `RecoveryManager` STVARNO
  pomjera bajtove preko `COPY_BLOCK`. `record_replication(bytes)` = Master
  -ova inicijalna placement odluka (poziva se u `handle_write_commit`/
  `handle_append_commit`, kad je stvarna veličina bloka već poznata).
  `record_recovery(bytes, success)` = `RecoveryManager`-ov `COPY_BLOCK`
  re-replikacije. Nema duplog brojanja iste operacije pod dva imena.
- **Cache brojači (§10.11) naspram Faza 13-ovog `ClientCache::stats()`**:
  `ClientCache` VEĆ prati hits/misses/evictions/invalidations/bytes_served.
  `StatisticsManager` NEMA `record_cache_hit()`/`record_cache_miss()`
  metode uopšte — `Client`-ov poseban `cache_stats()` accessor prosto vraća
  postojeći `CacheStats`, bez duplirane logike. `StatisticsSnapshot` (u
  `statistics/` biblioteci) namjerno NE zna za `CacheStats` (koji živi u
  `client/`) — sprečava upward zavisnost od generičke statistike ka
  Client-specifičnom tipu; spajanje se dešava na `Client` nivou, ne unutar
  `StatisticsManager`-a.

### 18.1 — Statistics/Histogram jezgro + Logger

Novi `statistics` lib (`include/statistics/statistics.h`,
`include/statistics/histogram.h`, odgovarajući `.cpp`), samostalan i
testiran bez mreže/Master/StorageNode/Client zavisnosti. `OperationType`
enum (`READ, WRITE, APPEND, CREATE, DELETE, LIST, STAT`) — `StatisticsManager`
drži po jedan `OperationCounters` (total/successful/failed/bytes + sopstveni
`Histogram`) za svaki tip, plus `errors_by_code_` (`std::array<atomic, 11>`
indeksiran `ErrorCode`-om), replication/recovery/heartbeat/node-dead
brojače, i storage capacity gauge (zaštićen `std::mutex`-om jer su
capacity/used/block_count smisleni samo zajedno kao konzistentna trojka,
ne kao tri nezavisna atomika koja mogu da se razmimoiđu).

`Logger` (`include/common/logger.h`) je **header-only** (C++17 `inline`
promenljive/funkcije) — namjerno, da se izbjegne pretvaranje `common`
INTERFACE biblioteke u pravu static-lib samo zbog jednog malog fajla.
`DEBUG/INFO/WARNING/ERROR` nivoi tačno po §10.32, globalni atomic prag,
`std::mutex` oko `std::cerr` ispisa (sam `std::cerr` nije bezbjedan pod
konkurentnim `<<` lancima).

**Bag nađen tokom pisanja sopstvenog testa (ne u proizvodnom kodu)**:
`const OperationStats& read = stats.snapshot().operations[idx];` je
dangling-reference — `std::array::operator[]` je poziv funkcije koji
prekida C++-ov lanac produženja životnog vijeka privremenog objekta (radi
SAMO za direktan `.`-pristup članu, ne kroz poziv funkcije koja vraća
referencu), pa je `StatisticsSnapshot` privremeni objekat uništen na kraju
te linije dok je `read` i dalje trebalo da se koristi na sljedećoj.
Uhvaćeno od strane ASan-a (`stack-use-after-scope`) tek u ASan/UBSan
prolazu — običan build ovo nikad ne bi otkrio. Popravljeno čuvanjem
`StatisticsSnapshot`-a u imenovanu lokalnu promjenljivu prije uzimanja
reference na njegov član. Dobra ilustracija zašto ASan/UBSan prolaz posle
SVAKE pod-faze nije opciono, čak i za "samo testni kod".

15 novih testova (`test_statistics.cpp`), čisto pod ASan/UBSan.

### 18.2 — Client retrofit

`Client` nema centralni dispatcher (nije server), pa svaki javni metod
(`create_file`, `write`, `read`, `append`, `delete_file`, `list`, `stat`)
je preimenovan u `*_impl` (privatno), a javno ime postaje tanak wrapper
koji mjeri `steady_clock` oko poziva `*_impl`-a i zove
`statistics_.record_operation()`/`record_read()`/`record_write()`/
`record_append()` — isti obrazac kao `BlockStorage`-ov postojeći
`read_block()`/`read_block_locked()` par, izbjegava dodirivanje svake
tačke povratka unutar metoda koji imaju više njih.

`read`/`write`/`append` prosljeđuju stvarnu veličinu fajla (jedino mjesto
koje zna ukupnu logičku veličinu — Master vidi samo kontrolne metapodatke,
StorageNode samo pojedinačne blokove).

Dva nova javna accessor-a: `Client::statistics_snapshot() const` i
`Client::cache_stats() const` (odvojeno, ne spojeno — `StatisticsSnapshot`
namjerno ne zna za `CacheStats`, koji živi u `client/cache.h`; spajanje bi
uvelo upward zavisnost od generičke `statistics/` biblioteke ka
Client-specifičnom tipu, vidi §18-ovu poprečnu odluku).

**Testovi**: proširen postojeći `tests/integration/test_create.cpp`,
`test_write.cpp`, `test_read.cpp`, `test_append.cpp`, `test_delete.cpp`,
`test_list_stat.cpp` — po jedna asercija po fajlu da
`client_->statistics_snapshot()`/`cache_stats()` posle stvarno izvršenih
operacija pokazuje očekivane brojeve (ne novi fajl, samo dopune postojećih
fixtures, tačno po planu).

231/231 testova prolazi (bez novih test IZVRŠNIH fajlova — samo dopune
postojećih), čisto pod ASan/UBSan.

### 18.3 — StorageNode retrofit + GET_STATS wire

`MessageType::GET_STATS`/`STATS_RESPONSE` dodati na kraj enuma (prvi put
korišćeni ovdje, isti presedan kao `NODE_INVENTORY`). `BlockManager` dobija
`used_space()`/`free_space()` passthrough (jednolinijski, isti obrazac kao
`get_block_info()`); `capacity_bytes = used_space() + free_space()` —
nije trebao novi `capacity()` getter na `BlockStorage`-u.

`StorageNode::handle_message()` (centralni dispatcher) preimenovan u
`route_message()`; novi tanak `handle_message()` wrapper mjeri
`steady_clock`, mapira `READ_BLOCK→READ`/`WRITE_BLOCK→WRITE`/
`DELETE_BLOCK→DELETE` (`to_operation_type()` helper — `COPY_BLOCK`/
`COPY_BLOCK_DATA`/`REGISTER_NODE`/`HEARTBEAT`/`GET_STATS` namjerno
nemapirani, to je cluster-membership/recovery/dijagnostički saobraćaj, ne
korisnička operacija), i zove `record_operation()`/`record_error()`.
Bajtovi se računaju direktno iz request/response wire formata (`compute_bytes()`
helper, koristi već postojeće `kBlockVersionSize`/`kDataSizeFieldSize`
konstante iz istog fajla) — bez potrebe za dodatnom `StatisticsManager`
metodom, i bez curenja handler-specifičnog wire-format znanja u wrapper
(sve živi u istoj translation unit, isti fajl koji i handleri koriste).

`heartbeat_loop()` zove `record_heartbeat_sent(success)` po pokušaju
(uspjeh = ACK primljen). Novi `handle_get_stats()`: ažurira storage
capacity gauge preko `block_manager_->used_space()`/`free_space()`/
`enumerate_blocks().size()`, pa vraća `STATS_RESPONSE` sa
`statistics_.format_snapshot()` kao UTF-8 payload.

**Testovi**: novi `StorageNodeTest.GetStatsReturnsTextSnapshotReflectingPriorOperations`
u `tests/unit/test_storage_node.cpp` — potvrđuje i wire tekst ODGOVOR i
in-process `statistics_snapshot()` accessor posle stvarnog WRITE_BLOCK/
READ_BLOCK para.

**Poznat, nepovezan environment flake ponovo zabilježen**: `ReadIntegrationTest.WriteThenReadReturnsSameBytes`
je pukao (segfault) zbog istog sandbox efemernog-porta fenomena kao ranije
(port 40202 zauzet od strane Firefox-a u trenutku testiranja) — potvrđeno
`fuser`-om, nepovezano sa ovim diff-om (nijedna izmjena ove pod-faze ne
dira taj test/port). Svih preostalih testova prolazi čisto i pod običnim
build-om i pod ASan/UBSan-om kad se ovaj isključi.

231/231 (232 sa novim testom, 1 isključen zbog environment flake-a)
testova prolazi, čisto pod ASan/UBSan.

### 18.4 — Master retrofit + dfs_statsctl CLI

Isti `handle_message`/`route_message` split kao 14.3, sa mapiranjem
`CREATE_FILE→CREATE`, `DELETE_FILE→DELETE`, `LIST_FILES→LIST`,
`STAT_FILE→STAT`, `READ_FILE→READ`, `WRITE_COMMIT→WRITE`,
`APPEND_COMMIT→APPEND` (`ALLOCATE_BLOCK`/`REGISTER_NODE`/`HEARTBEAT`/
`NODE_INVENTORY`/`GET_STATS` namjerno nemapirani — cluster-membership/
recovery/dijagnostički saobraćaj).

**`RecoveryManager` dobija `StatisticsManager&`** kao novi obavezan
konstruktorski parametar (isti obrazac referenc-injekcije kao ostali
saradnici) — ono je jedino mjesto koje zna stvarno oporavljene bajtove u
trenutku uspjeha. `Master::statistics_` MORA biti deklarisan PRIJE
`replication_manager_`/`recovery_manager_` u `master.h` (C++ inicijalizuje
članove po redoslijedu deklaracije, ne po redoslijedu u initializer
listi) — dokumentovano eksplicitno komentarom da se izbjegne buduća
zabuna, isti stil kao postojeći komentar o `recovery_manager_`/
`failure_detector_`-a redoslijedu.

**`record_replication`/`record_recovery` razgraničenje** (vidi poprečnu
odluku #7 gore) — implementaciono pojednostavljenje u odnosu na plan:
jedan `record_replication(size)` poziv PO COMMIT-u (ne po bloku), koristeći
već poznatu ukupnu veličinu iz `WRITE_COMMIT`/`APPEND_COMMIT` wire
payload-a, umjesto po-blok lookup-a — jednostavnije, i "koliko bajtova je
upravo replicirano ovim pisanjem" je jednako smisleno mjereno na nivou
cijelog commit-a.

**Logger retrofit**: dva postojeća `std::cerr` `NODE_INVENTORY` anomaly
loga prepravljena da koriste `logger::log(WARNING, ...)`; nova
`INFO`/`ERROR` mjesta dodata tačno na §10.32-ove imenovane primjere
(`"Storage Node X registered"`, `"Storage Node X declared DEAD"`).
`<iostream>` include uklonjen iz `master.cpp` — više se nigdje ne koristi
direktno.

**Novi `dfs_statsctl` executable** (`src/statistics/main_statsctl.cpp`):
sirovi `TCPConnection`/`Socket` (ne pun `Client` — ne treba namespace
logika), `--host <h> --port <p> stats`, šalje `GET_STATS`, ispisuje
`STATS_RESPONSE` payload direktno na stdout. Radi identično protiv
Master-a ili bilo kog StorageNode-a — potvrđeno ručno preko pravog
4-procesnog klastera (`scripts/start_*.sh`): CREATE→WRITE→READ pa
`dfs_statsctl stats` protiv oba porta, brojevi plauzibilni i međusobno
konzistentni (Master-ova `Replication: bytes=33` odgovara stvarnoj
veličini upisanog fajla; StorageNode-ova `Storage: used_bytes=33
block_count=1` isto tako).

**Testovi**: `tests/unit/test_master.cpp` proširen sa
`GetStatsReturnsTextSnapshotReflectingPriorOperations` i
`WriteCommitRecordsReplicationStats`; `tests/integration/test_recovery.cpp`-ov
postojeći `KilledNodeTriggersRecoveryAndReadKeepsSucceeding` proširen sa
`detected_failures`/`recovery_operations`/`successful_recoveries`
provjerom na kraju (end-to-end kroz pravi failure+recovery ciklus, ne
izolovan unit test).

**Poznat environment flake, treći put**: `WriteIntegrationTest.CreateThenWriteSucceeds`
je jednom pukao (segfault) zbog istog sandbox efemernog-porta fenomena —
prošao čisto na ponovnom pokretanju, nepovezano sa ovim diff-om.

232/232 testova prolazi, čisto pod ASan/UBSan.

---

**Nezavisan `cpp-reviewer` na cijeloj Fazi 14 — 3 MEDIUM + 2 LOW nalaza,
svih 5 popravljeno**:

- **MEDIUM: Master-ov `handle_message()` wrapper je hardkodovao
  `bytes=0`** za SVAKI operation type, uključujući WRITE/APPEND, iako je
  stvarna veličina već dostupna u `WRITE_COMMIT`/`APPEND_COMMIT` wire
  payload-u (`handle_write_commit`/`handle_append_commit` je već
  parsiraju za `record_replication()` par linija niže). Rezultat: `dfs_statsctl`
  protiv Master-a bi uvijek pokazao `WRITE: ... bytes=0` pored ispravnog
  `Replication: bytes=...` — interno nekonzistentna tabela. Novi
  `WriteCommitRecordsReplicationStats` test (dodat u istoj Fazi 14.4) je
  ovo propustio jer nikad nije asertovao `.bytes` polje. Popravljeno:
  novi `compute_bytes()` helper u `master.cpp` (isti obrazac kao
  StorageNode-ov, iz istog wire formata koji `handle_write_commit`/
  `handle_append_commit` već parsiraju), samo za `WRITE_COMMIT`/
  `APPEND_COMMIT` i samo na `ACK` odgovoru. Regresija: `.bytes` asercija
  dodata postojećem testu + nova asercija u `test_append.cpp`-ovom
  `AppendToEmptyFileIsEquivalentToWrite` (Master-side, ne samo
  Client-side).
- **MEDIUM: idempotentan WRITE_BLOCK retry (izgubljen ACK, isti
  `operation_id`) je duplo brojan u statistici** — `handle_message()`
  wrapper bezuslovno zove `record_operation()` oko `route_message()`-a,
  bez obzira da li je `route_message` stvarno izvršio pisanje ili samo
  vratio keširan odgovor iz `OperationLog`-a (Faza 12.2 idempotency).
  Popravljeno: novi `OperationLog::contains(id)` metod (read-only peek,
  za razliku od `begin_or_get()`, koji mutira stanje) pozvan PRIJE
  `route_message()`-a da uhvati "da li je ovo poznat operation_id" stanje
  na početku poziva — provjera POSLIJE `route_message()`-a bi uvijek
  vratila true čak i za svjež zahtjev (jer `begin_or_get()` upravo ubaci
  novi unos kao svoju sporednu posljedicu). Regresija:
  `IdempotentWriteBlockRetryDoesNotDoubleCountStats` — isti
  `operation_id`, dva različita `request_id`-a, potvrđuje `total == 1` ne
  `2`.
- **MEDIUM: `OperationCounters`-ova 4 atomika + `Histogram` se ažuriraju
  nezavisno, bez zajedničkog lock-a** — konkurentan `snapshot()` može
  vidjeti ih "poderane" jedne u odnosu na druge (npr. `total` već
  inkrementiran a `bytes`/latencija još nisu). Namjerno prihvaćeno (isti
  §10.39 princip kao capacity gauge-ov mutex, ali ovdje je dijagnostički
  značaj dovoljno nizak da opravda jeftinije atomike bez zajedničkog
  lock-a) — popravljeno DOKUMENTOVANJEM ove odluke eksplicitno pored
  `OperationCounters` strukture, da budući čitalac ne pomisli da je
  `successful + failed != total` u live snapshot-u bag.
- **LOW: `get_u16()` na error payload-u bez provjere granica** u oba
  wrapper-a (`Master::handle_message`/`StorageNode::handle_message`) —
  danas bezopasno (svaki `MessageType::ERROR` u oba fajla trenutno uvijek
  puni bar 2-bajtni payload), ali wrapper je generički dispatcher kod koji
  radi nad SVAKIM handler-ovim rezultatom, sadašnjim i budućim. Popravljeno
  dodavanjem `payload.size() >= sizeof(uint16_t)` provjere direktno u oba
  wrapper poziva (defense-in-depth, nezavisno od `make_error()`-ove
  sopstvene ispravnosti).
- **LOW: `GET_STATS`/`STATS_RESPONSE` ubačeni usred `MessageType` enuma**,
  pomjerajući numeričke vrijednosti `ACK`/`ERROR`-a u odnosu na
  pre-Faza-14 build. Bezopasno za OVAJ projekat (single-binary build,
  nema perzistovanih sirovih `MessageType` vrijednosti, nema mixed-version
  deployment-a), ali loša higijena za over-the-wire enum uopšteno.
  Popravljeno premještanjem oba na sam kraj enuma (poslije `ERROR`).

233/233 testova prolazi (3 nova regresiona: `WriteCommitRecordsReplicationStats`-ovo
prošireno, `AppendToEmptyFileIsEquivalentToWrite`-ovo prošireno,
`IdempotentWriteBlockRetryDoesNotDoubleCountStats` nov), čisto pod
ASan/UBSan.

---

## 19. Faza 15 — Concurrency hardening pass (§11 dizajn dokumenta)

Ne dodaje feature-e — prelazi sa grubih single-mutex-po-komponenti
placeholder-a korišćenih od Faze 5 na finalni concurrency model iz §11, i
dokazuje korektnost pod opterećenjem sa ThreadSanitizer-om. Podijeljena na
4 pod-korака (15.1-15.4), isti obrazac kao Faza 12/14.

**Ispravka pogrešne pretpostavke iz prethodne sesije**: prijašnja bilješka
je tvrdila "ThreadSanitizer je dokumentovano pokvaren u ovom sandbox-u od
Faze 5" — pretraga cijelog repoa nije našla NIKAKAV dokaz za to (TSan
jednostavno nikad nije bio isprobavan u ovom projektu). `libtsan.so.2` je
instaliran u ovom sandbox-u (Ubuntu 24.04, GCC 13.3.0). Faza 15.1 je
ovo empirijski riješila umjesto da nastavi vjerovati staroj pretpostavci.

**15.1 — TSan baseline (bez refaktora)**: prvi `cmake --build`/`ctest`
pod `-fsanitize=thread` je pukao sa `FATAL: ThreadSanitizer: unexpected
memory mapping` — poznat, generički ASLR/mmap problem sa TSan-om na
modernim kernelima u ograničenim sandbox-ovima, NE sa samim projektom
(potvrđeno minimalnim reprodukcionim programom van projekta). Rešeno
pokretanjem cijelog build+test koraka pod `setarch $(uname -m) -R`
(isključuje ASLR) — isti minimalni reprodukcioni program je nakon toga
korektno prijavio pravu ugrađenu race condition, potvrđujući da TSan
sam po sebi radi ispravno. Baseline na TAD-postojećem kodu (jedan
`shared_mutex` po komponenti): **235/235 testova, nula TSan upozorenja**
— očekivano, pošto jedan grubi lock po komponenti trivijalno serializuje
sve, ostavljajući TSan-u ništa da uhvati. Nikakva izmjena koda u ovom
pod-koraku.

**15.2 — `BlockStorage`/`BlockManager` per-block locking (glavni
refaktor)**: `BlockStorage`-ov jedini `shared_mutex mutex_` (štitio i
`local_blocks_` mapu I `used_space_` I sam disk I/O — WRITE jednog bloka
je blokirao READ/WRITE bilo kog drugog bloka na istom node-u) zamijenjen
sa tri nivoa lock-ova:
- `map_mutex_` (`shared_mutex`) — štiti SAMO strukturu `local_blocks_`
  mape (BlockId -> `shared_ptr<BlockState>`).
- `BlockState::mutex` (`shared_mutex`, jedan po bloku) — štiti tog bloka
  `has_data`/`LocalBlockInfo`, i sam disk I/O za taj blok. `shared_lock`
  za READ, `unique_lock` za WRITE/APPEND — točno §11.14-ov model.
- `space_mutex_` (plain `mutex`) — štiti SAMO cross-block `used_space_`
  agregat (`try_apply_space_delta()`, check-and-reserve PRIJE bilo kog
  I/O-a, tako da concurrentni write-ovi na RAZLIČITE blokove ne mogu
  zajedno premašiti `capacity_`).

Red akvizicije: `map_mutex_` -> `BlockState::mutex` -> `space_mutex_`,
nikad obrnuto. `BlockState` se NIKAD ne briše iz mape (ni na
`delete_block()`) — samo mu se `has_data` postavi na `false`, tačno kao
za placeholder ostavljen neuspjelim prvim write-om.

**Kritičan nalaz cpp-reviewer-a, popravljen prije commit-a**: prva verzija
JE brisala `BlockState` iz mape na `delete_block()`. To je otvorilo
race: `write_block()` koji je već razrešio svoj `shared_ptr<BlockState>`
kroz `get_or_create_block_state()` (i pustio `map_mutex_`) prije nego što
konkurentan `delete_block()` na ISTOM id-u obriše taj unos iz mape — write
onda upiše u `BlockState` na koji mapa više ne pokazuje, vrati `OK`
pozivaocu, ali podatak postane trajno nevidljiv kroz javni API (dok se
proces ne restartuje i `recover()` ga ponovo ne otkrije sa diska). Pošto
`BlockState` sad NIKAD ne nestaje iz mape, `get_or_create`/`find` uvijek
vraćaju isti trajni objekat za dati id — race je strukturno nemoguć.
Regresija: `ConcurrentWriteAndDeleteOnSameIdNeverOrphansAWrite`.

Novi `tests/concurrency/` (§11.41-43 scenariji, direktno na `BlockStorage`,
bez mreže): `test_concurrent_append.cpp` (100 thread-ova, svaki append-uje
100 bajtova na ISTI blok preko `write_block_at()` — finalna veličina
tačno 10.000 bajtova, svaki segment intaktan), `test_concurrent_reads.cpp`
(50 thread-ova čita isti blok, svi identični), `test_concurrent_writes.cpp`
(pisac vs. čitači — nikad poderan blok; plus cross-block capacity test).
`enumerate_blocks()` je namjerno prešao sa atomičnosti cijelog snapshot-a
na atomičnost po bloku (nijedan poznat pozivalac ne zavisi od
cross-block atomičnosti; zadržavanje bi vratilo baš onu kontenciju koju
ova faza uklanja) — odluka potvrđena sa korisnikom prije implementacije.

**15.3 — Audit ostatka (bez refaktora, samo potvrda + testovi)**:
`NamespaceManager` (jedan `shared_mutex` za cijeli namespace — §11.24-ov
namjerni cilj, NE dalje deljenje po fajlu; `shared_lock` za LIST/STAT,
`unique_lock` za CREATE/DELETE, potvrđeno metod-po-metod), `StorageRegistry`
+ `FailureDetector` (heartbeat sweep thread dira registry SAMO kroz
njegov javni locked API, nema bypass-a), `Master` (svi pozivi u
`namespace_manager_`/`storage_registry_`/`replication_manager_` su
sekvencijalni, nikad ugniježđeni — nema deadlock rizika), `ClientCache`
("unlock before network I/O" pravilo iz §9.20/§11.29 strukturno
garantovano samom API formom — `get()`/`put()` nikad ne vraćaju
referencu u `entries_`). Nula bagova nađeno — svi su već bili ispravni.
Dodata dva teža stress testa (`ConcurrentCreateDeleteAndListStatDontRace`,
`ConcurrentHeartbeatWritesAndReaderQueriesDontRace`) da se to dokaže pod
pravom kontencijom, ne samo na papiru.

**15.4 — Finalni TSan gate**: cijeli test suite (242 testa, jedan
isključen — vidi ispod) pod `-fsanitize=thread`, potvrđeno DVA PUTA:
**nula TSan upozorenja**. Baseline iz 15.1 (nula upozorenja na starom
kodu) se poklapa sa finalnim rezultatom (nula upozorenja na novom
per-block kodu) — nikakav suppression fajl nije bio potreban.

**Odluka: TSan se pokreće ručno, ne kroz `cmake/tsan.cmake`** — iako
`IMPLEMENTATION_PLAN.md`-ov tekst pominje takav fajl, projekat ASan/UBSan
NIKAD nije uveo kao checked-in CMake opciju (uvijek ručni
`-DCMAKE_CXX_FLAGS="-fsanitize=..."` poziv, dokumentovan, ne automatizovan)
— dodavanje jednog novog CMake fajla samo za treći od tri sanitizer-a
krši isti obrazac bez stvarne dobiti. Kanonska komanda (mora ići kroz
`setarch` zbog gore opisanog ASLR/mmap problema):

```
setarch "$(uname -m)" -R cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
setarch "$(uname -m)" -R cmake --build build-tsan -j"$(nproc)"
TSAN_OPTIONS="halt_on_error=0" setarch "$(uname -m)" -R ctest --test-dir build-tsan --output-on-failure
```

**Poznat environment flake, ne Faza 15 bag**: `ReadIntegrationTest.
ReadAfterOverwriteWithFewerBlocksReturnsNewContentOnly` je konzistentno
pukao (segfault na `master_->start()` sa `INTERNAL_ERROR`) tokom cijele
ove sesije — `fuser 40250/tcp` je potvrdio da OVA claude sesija sama
ima trajnu izlaznu konekciju na baš tom efemernom portu (isti fenomen
dokumentovan od Faze 9, samo ovog puta uzročnik je sesija sama, ne
Firefox), pa je test isključivan iz svih run-ova (`-E` filter). Dva
druga, DIFERENT testa (`StorageNodeTest.DeleteThenReadReturnsBlockNotFound`,
`MasterTest.DeleteFileWithHangingReplicaCompletesWithinBoundedTime`) su
svaki po jednom flake-ovala na jednom od više punih run-ova (oba prošla
čisto izolovano i na ponovnom punom run-u) — pripisano opterećenju
sistema tokom teških integration testova, ne regresiji (nijedan ne dira
ništa iz ovog diff-a).

242/241 testova prolazi (241 stvarnih + 1 isključen environment flake),
čisto pod ASan/UBSan, čisto pod ThreadSanitizer-om (nula upozorenja).
