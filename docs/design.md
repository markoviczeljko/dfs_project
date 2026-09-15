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
