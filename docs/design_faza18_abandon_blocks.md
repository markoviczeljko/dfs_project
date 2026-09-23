# Faza 18 — ABANDON_BLOCKS: čišćenje siročadi blokova nakon djelimično neuspjelog WRITE-a

**Status: IMPLEMENTIRANO (2026-09-23).** Implementacija prati ovaj nacrt uz
proširenja opisana u `docs/design.md` §27: abandon na svim tačkama otkaza
(uključujući neuspio `WRITE_COMMIT`/`APPEND_COMMIT`), samostalno čišćenje u
`Master::handle_allocate_block()`, zaostali pending id-jevi kao orphan-i u
`commit_write()`, i paralelni fan-out u `Client::write_chunk_to_replicas()`.
Nacrt ispod je ostavljen kao izvorni dizajnerski zapis.

## 1. Problem

Kada višedjelni (multi-chunk) `Client::write()` otkaže na nekom kasnijem
dijelu (npr. treći od pet blokova ne uspije da se upiše na sve dodijeljene
replike), blokovi koji su USPJEŠNO upisani na replike prije tog otkaza
ostaju zauvijek:

- u Masterovom `NamespaceManager::blocks_` mapi, u stanju `ALLOCATED`
  (`namespace.cpp:245`, `set_block_replicas()`), jer `WRITE_COMMIT` nikad ne
  stiže da bi ih potvrdio niti obrisao;
- kao stvarni `.block` fajlovi na disku storage čvorova
  (`block_storage.cpp:241-269`, `write_block_locked()` upisuje i rezerviše
  prostor čim primi `WRITE_BLOCK`, nezavisno od ishoda ostatka operacije).

Dodatno, ako klijent kasnije *retry*-uje isti path i uspije,
`NamespaceManager::commit_write()` briše CIJEL `pending_blocks_[path]` skup
(ne samo upravo committovane id-jeve) — prvi, neuspjeli pokušaj tiho
iščezava iz `pending_blocks_` a da njegov `blocks_[id]` zapis nikad nije
očišćen. Ovo je isti bug, samo teže uočljiv (ne vidi se ni u
`pending_blocks_` poslije).

## 2. Zašto postojeći orphan-detection mehanizam (NODE_INVENTORY) ovo ne rješava

`Master::handle_node_inventory()` (`master.cpp` ~558-570) obrađuje
`NODE_INVENTORY` poruke i za blokove koje Master uopšte ne poznaje
("pravi orphan") samo **loguje**, nikad ne briše — namjerna, već
dokumentovana i u tezi obrazložena odluka
(`poglavlja/03_implementacija.tex`, §Опоравак Storage чвора и
реконсилијација инвентара, ~linija 403-411 i ~450), potkrijepljena
postojećim testom `NodeInventoryLogsUnknownBlockAsOrphanWithoutDeleting`
(`tests/unit/test_master.cpp:1168`).

Dva razloga zašto taj put ionako ne pokriva OVAJ bug, čak i kad bi brisao
umjesto logovao:

1. **`NODE_INVENTORY` nije periodičan.** Šalje se samo pri
   `REGISTER_NODE` ili nakon restarta čvora/Mastera (`needs_inventory` bit,
   postavljen samo dok je stanje čvora `JOINING`) — ne na svaki heartbeat.
   U dugotrajno stabilnom klasteru se možda nikad više ne pokrene.
2. **Blok iz ovog buga nije "pravi orphan" po definiciji tog koda.** Master
   i dalje ima tačan `blocks_[id]` zapis (stanje `ALLOCATED`, tačne
   replike) — `block_metadata()` uspijeva, node se nalazi u
   `metadata.value().replicas`, kod upada u "reported_version ==
   known_version" granu i ne radi ništa, nikad ne loguje niti flaguje.

**Zaključak: potreban je nov, poseban mehanizam** (Approach A —
client-triggered abandon), bez diranja `NODE_INVENTORY` puta.

## 3. Protokol: novi tip poruke

`include/network/protocol.h` — dodati na kraj `MessageType` enum-a (ne
umetati usred, radi očuvanja postojećih numeričkih vrijednosti na žici):

```cpp
// Faza 18: client-side cleanup signal poslije djelimično neuspjelog
// multi-chunk WRITE-a — vidi docs/design_faza18_abandon_blocks.md
ABANDON_BLOCKS,
```

Wire format zahtjeva (payload): `[u32 path_len][path][u32 block_count][block_count x u64 block_id]`.

## 4. `NamespaceManager::abandon_blocks()`

`include/master/namespace.h`, blizu `allocate_block()`:

```cpp
// Faza 18: poništava dio efekta allocate_block()-a za id-jeve koje je
// WRITE pokušaj napustio poslije otkaza nekog chunk-a, prije nego što je
// ikad stigao do commit_write()-a. Dira samo id-jeve koji su i dalje
// prisutni u pending setu za dati path — id koji više nije pending (već
// committovan, ili već odbačen nekim kasnijim commit_write()/delete_file()
// na istom path-u) se tiho preskače, nikad kao greška: block id-jevi se
// nikad ne recikliraju (next_block_id_ striktno raste), pa id koji ovdje
// nije pending nikad neće legitimno zatrebati da ga ovaj poziv ponovo
// obriše. Vraća OrphanedBlock snapshot (id + replike) za one id-jeve koji
// SU bili pending, identično kao delete_file()/commit_write(), tako da
// Master može isti gc_dispatch() da ih obriše.
Result<std::vector<OrphanedBlock>> abandon_blocks(
    const std::string& path, const std::vector<BlockId>& block_ids);
```

`src/master/namespace.cpp`:

```cpp
Result<std::vector<OrphanedBlock>> NamespaceManager::abandon_blocks(
    const std::string& path, const std::vector<BlockId>& block_ids) {
    auto normalized_result = normalize_path(path);
    if (normalized_result.is_error()) {
        return Result<std::vector<OrphanedBlock>>::err(normalized_result.error());
    }
    const std::string& normalized = normalized_result.value();

    std::unique_lock lock(mutex_);
    std::vector<OrphanedBlock> orphans;
    auto pending_it = pending_blocks_.find(normalized);
    if (pending_it == pending_blocks_.end()) {
        return Result<std::vector<OrphanedBlock>>::ok(std::move(orphans));  // ništa pending — no-op
    }
    for (BlockId id : block_ids) {
        auto set_it = pending_it->second.find(id);
        if (set_it == pending_it->second.end()) {
            continue;  // već committovan/odbačen negdje drugo — idempotentan preskok
        }
        pending_it->second.erase(set_it);
        auto block_it = blocks_.find(id);
        if (block_it != blocks_.end()) {
            orphans.push_back(OrphanedBlock{id, block_it->second.replicas});
            blocks_.erase(block_it);
        }
    }
    if (pending_it->second.empty()) {
        pending_blocks_.erase(pending_it);
    }
    return Result<std::vector<OrphanedBlock>>::ok(std::move(orphans));
}
```

Namjerno **bez** `files_.find()`/`FILE_NOT_FOUND` provjere (za razliku od
`allocate_block()`) — funkcioniše i ako je path u međuvremenu obrisan od
drugog klijenta. Uvijek `ok()`, nikad `FILE_NOT_FOUND` greška, dosljedno sa
`handle_node_inventory()`-jevom "uvijek ACK" konvencijom.

## 5. `Master::handle_abandon_blocks()`

`include/master/master.h` — dodati u routing:

```cpp
Message handle_abandon_blocks(const Message& request);
```

`src/master/master.cpp`, `route_message()` (blizu `NODE_INVENTORY` case-a):

```cpp
case MessageType::ABANDON_BLOCKS:
    return handle_abandon_blocks(request);
```

Implementacija (blizu `handle_write_commit()`):

```cpp
Message Master::handle_abandon_blocks(const Message& request) {
    // Wire format: [u32 path_len][path][u32 block_count][block_count x u64 block_id].
    // Faza 18: best-effort cleanup signal koji Client šalje odmah nakon
    // što multi-chunk WRITE otkaže usred operacije, za svaki block id koji
    // je ALLOCATE_BLOCK-ovao tokom tog pokušaja (uključujući i onaj čiji
    // upis nije uspio) — vidi docs/design_faza18_abandon_blocks.md. Uvijek
    // ACK, isto kao NODE_INVENTORY — klijentov upis je već propao, nema
    // smisla da i cleanup propadne za njega.
    if (request.payload.size() < sizeof(uint32_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    uint32_t path_length = get_u32(request.payload.data());
    size_t offset = sizeof(uint32_t);
    if (request.payload.size() < offset + path_length + sizeof(uint32_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    std::string path(reinterpret_cast<const char*>(request.payload.data() + offset), path_length);
    offset += path_length;
    uint32_t block_count = get_u32(request.payload.data() + offset);
    offset += sizeof(uint32_t);
    if (request.payload.size() != offset + static_cast<size_t>(block_count) * sizeof(uint64_t)) {
        return make_error(request.request_id, ErrorCode::INVALID_REQUEST);
    }
    std::vector<BlockId> block_ids;
    block_ids.reserve(block_count);
    for (uint32_t i = 0; i < block_count; ++i) {
        block_ids.push_back(get_u64(request.payload.data() + offset));
        offset += sizeof(uint64_t);
    }

    auto result = namespace_manager_.abandon_blocks(path, block_ids);
    if (result.is_error()) {
        return make_error(request.request_id, result.error());
    }
    gc_dispatch(result.value());
    // NEMA persist_metadata(): pending_blocks_/ne-committovani blocks_
    // zapisi se nikad ne perzistuju (vidi namespace.h:143-148,
    // restore()-ov komentar) — nema ništa novo za snimiti.
    return Message{MessageType::ACK, request.request_id, {}};
}
```

## 6. Client strana

`include/client/client.h`, privatni helper:

```cpp
// Faza 18: best-effort cleanup poslije write_new_blocks()-ovog prekida
// usred multi-chunk upisa — javlja Masteru koje je block id-jeve ovaj
// pokušaj alocirao (uključujući onaj čiji je upis upravo otkazao) da ih
// odmah oslobodi umjesto da procure do nekog kasnijeg, nepovezanog
// događaja. Nikad ne mijenja šta write_new_blocks() vraća pozivaocu —
// fire-and-forget, sopstvena greška se tiho guta.
void abandon_blocks(const std::string& path, const std::vector<BlockId>& block_ids);
```

`src/client/client.cpp`:

```cpp
void Client::abandon_blocks(const std::string& path, const std::vector<BlockId>& block_ids) {
    if (block_ids.empty()) return;
    std::vector<std::byte> payload;
    put_string(payload, path);
    put_u32(payload, static_cast<uint32_t>(block_ids.size()));
    for (BlockId id : block_ids) {
        put_u64(payload, id);
    }
    // Best-effort: ishod se potpuno ignoriše. Ako i ovo ne uspije (Master
    // nedostupan, Client se sprema da se sruši, itd.) blokovi jednostavno
    // ostaju siročad — ništa gore nego bez ovog fixa, nikad eskalirano
    // pozivaocu kao greška.
    call_master(Message{MessageType::ABANDON_BLOCKS, next_request_id(), std::move(payload)});
}
```

U `write_new_blocks()` (`src/client/client.cpp:333-384`), na tačci otkaza:

```cpp
ErrorCode write_err =
    write_chunk_to_replicas(block_id, /*offset=*/0, block_version, data + chunk_offset,
                             chunk_len, replicas_result.value());
if (write_err != ErrorCode::OK) {
    block_ids.push_back(block_id);          // NOVO: uključiti i id chunk-a koji je otkazao —
                                             // Master ga je alocirao iako upis nije uspio
    abandon_blocks(path, block_ids);        // NOVO
    return Result<std::vector<BlockId>>::err(write_err);
}
```

`append_impl()` (`client.cpp:548`) interno poziva isti `write_new_blocks()`
za "novi blokovi" rep append-a, pa automatski nasljeđuje fix — provjeriti
pri implementaciji da ne guta/reinterpretira grešku na način koji bi
preskočio ovaj put.

## 7. Edge slučajevi

| Slučaj | Ponašanje | Zašto je bezbjedno |
|---|---|---|
| Klijent se sruši prije slanja `ABANDON_BLOCKS` | Blok ostaje siroče, isto kao danas | Prihvaćen, dokumentovan preostali gap — van obima ovog fixa |
| Storage čvor nedostupan tokom cleanup-a | Pokriveno postojećim `gc_dispatch()`/`send_delete_block()` timeout+skip (2000ms, best-effort) | Dosljedno sa `DELETE_FILE` politikom, bez novog retry mehanizma |
| Isti block id "recikliran" kasnije | Nemoguće | `next_block_id_` striktno raste, resetuje se samo naviše pri restartu (iz snapshot-a) |
| Race: path committovan/obrisan drugom operacijom dok je `ABANDON_BLOCKS` u letu | Tiho preskače id-jeve koji više nisu pending | Nema duplog brisanja, nema greške |
| Master restart između `ALLOCATE_BLOCK` i `ABANDON_BLOCKS` | Master je već zaboravio alokaciju (nije perzistovana) → no-op ACK | Bezopasno |

## 8. Testovi (TDD — pisati PRIJE koda)

Nijedan postojeći test trenutno ne pokriva djelimično neuspjeli multi-chunk
write (potvrđeno grep-om za `write_new_blocks`/`Abandon`/`partial` u
`tests/`).

**8.1 `tests/unit/test_namespace.cpp`** — direktni testovi za
`NamespaceManager::abandon_blocks()`:
- `AbandonBlocksRemovesIdsFromPendingSet`
- `AbandonBlocksReturnsOrphanedBlockWithCorrectReplicas`
- `AbandonBlocksOnUnknownPathIsNoOpNotError`
- `AbandonBlocksIgnoresIdsAlreadyCommittedElsewhere`
- `AbandonBlocksIsIdempotent`

**8.2 `tests/unit/test_master.cpp`** — testovi za `ABANDON_BLOCKS` handler
(koristiti postojeću `RecordingNode`/`register_three_recording_nodes`
infrastrukturu, blizu "Faza 11: DELETE_FILE GC dispatch" bloka ~linija 992
i `NodeInventory...` bloka ~linija 1166):
- `AbandonBlocksDispatchesDeleteBlockToAllRegisteredReplicas`
- `AbandonBlocksDoesNotAffectAlreadyCommittedBlocks`
- `AbandonBlocksOnUnknownPathReturnsAckWithoutDispatch`
- `AbandonBlocksWithUnreachableReplicaStillReturnsAckToClient` (mirror
  `DeleteFileWithUnreachableReplicaStillReturnsAckToClient`, linija 1020)
- malformed-payload varijante (truncated `block_count`, size mismatch) →
  `INVALID_REQUEST`

**8.3 `tests/integration/test_write.cpp`** (ili novi
`test_write_failure.cpp`, po uzoru na `block_exists_on_disk()` iz
`tests/integration/test_delete.cpp:69-75`):
- `MultiChunkWriteFailureDoesNotLeakBlocksOnDisk` — 4 storage čvora, RF=3,
  mali `block_size` (payload ≥2 čanka); ugasiti 4. čvor prije upisa tako
  da prvi čank uspije na sve 3 žive replike a drugi čank otkaže odmah
  (deterministički round-robin izbor replika,
  `replication_manager.cpp:8-24` — cursor 0 → chunk 0 dobija `{1,2,3}`,
  cursor 3 wraps → chunk 1 dobija `{4,1,2}`, prvi u listi je mrtav čvor
  4). Nakon neuspjelog `write()`, provjeriti da na diskovima živih čvorova
  **nema** ostavljenih `.block` fajlova (test bi pao prije fixa, prolazi
  poslije). Fallback ako 4-čvor trik nije stabilan u praksi: 3 čvora +
  gašenje jednog prije upisa na ciljni path (uži, ali validan regresioni
  test).

**8.4** Postojeći `NodeInventoryLogsUnknownBlockAsOrphanWithoutDeleting`
(`test_master.cpp:1168`) se **ne dira** — ovaj fix namjerno ne mijenja taj
kod put (§2).

Provjeriti pokrivenost novog/izmijenjenog koda (gcovr/lcov, po postojećoj
konvenciji projekta) prije nego se FAZA 3 smatra završenom — kućno pravilo
80%+.

## 9. Kritični fajlovi

- `include/network/protocol.h`
- `include/master/namespace.h`, `src/master/namespace.cpp`
- `include/master/master.h`, `src/master/master.cpp`
- `include/client/client.h`, `src/client/client.cpp`
- `tests/unit/test_namespace.cpp`
- `tests/unit/test_master.cpp`
- `tests/integration/test_write.cpp` (ili novi `test_write_failure.cpp`)

## 10. Redoslijed implementacije (FAZA 3, kad bude odobrena)

1. Testovi iz §8 (moraju pasti bez fixa — dokaz da test stvarno testira bug).
2. `abandon_blocks()` u `NamespaceManager` (§4).
3. `ABANDON_BLOCKS` protokol + `Master::handle_abandon_blocks()` (§3, §5).
4. Client strana (§6).
5. Build + pun test suite (`tests/unit/`, `tests/integration/`,
   `tests/failure/`) — svi testovi, uključujući nove, moraju proći.
6. Provjera pokrivenosti (80%+).
