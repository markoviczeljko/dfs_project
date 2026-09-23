# Faza 19 — Revalidacija keša na pogotku (versioned re-read on cache hit)

**Status: DIZAJN, NIJE IMPLEMENTIRANO.** Ovaj dokument je izvršni plan za
kod — namjerno ne sadrži izmjene `.cpp`/`.h` fajlova. Implementacija čeka
eksplicitan signal, isto kao Faza 18 (`docs/design_faza18_abandon_blocks.md`).
**Za razliku od Faze 18, ovaj dokument NE preporučuje da se tekst
diplomskog rada opiše kao već-implementiran prije stvarne implementacije —
razlog je u §7 (Uticaj na eksperimentalne rezultate), pročitati prije
donošenja odluke.**

## 1. Problem (mentorov komentar #4)

`Client::read()` (`src/client/client.cpp:695-698`) na pogotku u kešu vraća
keširane podatke bez ijednog mrežnog poziva:

```cpp
if (auto cached = cache_.get(block.block_id); cached.has_value()) {
    result.insert(result.end(), cached->data.begin(), cached->data.end());
    continue;
}
```

Ovo je ispravno **samo** ako je pretpostavka jedinog pisca (`docs/design.md`
§17, `poglavlja/03_implementacija.tex` §Кеширање, linije 176-181) tačna. Ako
klijent A keš-uje blok, a klijent B ga naknadno izmijeni, A o tome ne
dobija nikakvo obavještenje — čita zastario sadržaj sve dok se njegov keš
ne isprazni po LRU pritisku ili dok sam A ne upiše na taj blok. Mentor
pita da li se ovo može popraviti.

## 2. Zašto je ovo jeftinije od punog invalidation-broadcast mehanizma

Protokol **već** ima sve što je potrebno za jeftinu (conditional) reviziju,
implementirano i testirano na strani `Storage` čvora još od Faze 13:

- `BlockStorage::read_block_if_changed(id, known_version)`
  (`include/storage/block_storage.h:79-90`, `src/storage/block_storage.cpp:431-454`)
  — vraća `not_modified=true` (bez podataka) kad `known_version` odgovara
  trenutnoj verziji na disku; `known_version == 0` nikad ne poklapa (rezervisan
  kao "ništa keširano" sentinel).
- `StorageNode::handle_read_block()` (`src/storage/storage_node.cpp:420-455`)
  — vraća `NOT_MODIFIED` (samo `[u64 version]`, bez podataka) ili `READ_OK`
  (`[u64 version][u32 size][podaci]`), zavisno od ishoda gornje provjere.
- `ClientCache::CacheEntry` (`include/client/cache.h:19-23`) **već čuva**
  `version` po unosu — polje postoji, samo se trenutno ne koristi pri
  čitanju iz keša.

Jedina nedostajuća komponenta je na **klijentskoj** strani: `read()` mora,
na pogotku, poslati `READ_BLOCK` sa `known_version = cached->version`
umjesto da odmah vrati podatke — nema novog tipa poruke, nema promjene
wire formata, nema izmjena na `Master`-u ili `Storage` čvoru.

## 3. Izmjena: `Client::read()` (`src/client/client.cpp:690-747`)

Trenutni tok na pogotku (`continue`, nikad mreža) zamjenjuje se tokom
identičnim postojećoj petlji za promašaj, samo sa `known_version =
cached->version` umjesto `0`, i sa granom za `NOT_MODIFIED`:

```cpp
if (auto cached = cache_.get(block.block_id); cached.has_value()) {
    bool revalidated = false;
    for (const Location& location : block.replicas) {
        auto socket_result = Socket::create();
        if (socket_result.is_error()) continue;
        Socket socket = std::move(socket_result.value());
        if (socket.connect(location.host, location.port) != ErrorCode::OK) continue;
        TCPConnection node_connection(std::move(socket));

        std::vector<std::byte> read_payload;
        put_u64(read_payload, block.block_id);
        put_u64(read_payload, cached->version);  // NOVO: pravi known_version umjesto 0
        if (!node_connection.sendMessage(
                Message{MessageType::READ_BLOCK, next_request_id(), std::move(read_payload)})) {
            continue;
        }
        auto read_response = node_connection.receiveMessage();
        if (!read_response.has_value()) continue;

        if (read_response->type == MessageType::NOT_MODIFIED) {
            // Keš je i dalje tačan — koristi postojeće podatke, ne prenosi ih ponovo.
            result.insert(result.end(), cached->data.begin(), cached->data.end());
            revalidated = true;
            break;
        }
        if (read_response->type == MessageType::READ_OK) {
            // Promijenjeno od strane drugog klijenta — osvježi keš i koristi svježe podatke.
            // [parsiranje payload-a identično postojećem miss-putu, linije 726-738]
            cache_.put(block.block_id, version, block_data);
            result.insert(result.end(), block_data.begin(), block_data.end());
            revalidated = true;
            break;
        }
        // ERROR ili nepoznat tip — pokušaj sljedeću repliku.
    }
    if (revalidated) continue;
    // NOVO — vidi §4: šta ako revalidacija ne uspije ni na jednoj replici.
    cache_.invalidate(block.block_id);
    // pada kroz u postojeću miss-petlju ispod, sa known_version=0.
}
```

## 4. Dizajnerska odluka: šta kad revalidacija ne uspije (sve replike nedostupne)

Dvije opcije, obje bezbjedne, biraju se prema istom principu koji sistem
već dosljedno koristi za DELETE/write-failure putanje (best-effort,
nikad tiho pogrešan rezultat):

| Opcija | Ponašanje | Kompromis |
|---|---|---|
| **A — fail open (preporučeno)** | Ako nijedna replika ne odgovori na conditional read, invalidiraj lokalni unos i padni u postojeću miss-petlju (pun fetch sa `known_version=0`) | Ako i miss-petlja otkaže (sve replike stvarno nedostupne), read ionako vraća `BLOCK_NOT_FOUND` kao i danas — nikad se ne vraćaju tiho zastarali podaci kad je mreža dostupna, a dostupnost degradira identično postojećem ponašanju kad nije |
| B — fail to stale | Ako revalidacija ne uspije, vrati stare keširane podatke bez greške | Veća raspoloživost pri kratkim mrežnim zastojima, ali sistem tada MOŽE tiho vratiti zastario sadržaj bez ikakve naznake — suprotno duhu ovog fixa (mentor pita upravo za ovaj slučaj) |

Opcija A je dosljedna sa ostatkom sistema (npr. `DELETE_FILE`/`gc_dispatch()`
je best-effort ali nikad ne laže o ishodu operacije koja klijenta zanima) i
directno adresira mentorov komentar bez novog, tihog failure moda.

## 5. `ClientCache` — nema potrebnih izmjena API-ja

`get()` već vraća `version` kao dio `CacheEntry` (`cache.h:19-23`); `put()`
već prima `version`. Revalidacija koristi postojeći API bez izmjena
potpisa. Jedina nova upotreba: `invalidate()` se sada poziva i sa
klijentske strane pri neuspjeloj revalidaciji (§4, Opcija A), ne samo
poslije sopstvenog uspješnog upisa kao danas.

## 6. Edge slučajevi

| Slučaj | Ponašanje | Zašto je bezbjedno |
|---|---|---|
| Blok nepromijenjen | `NOT_MODIFIED`, keš se koristi bez ponovnog prenosa podataka | Isti podaci kao danas, samo jedan mali mrežni round-trip umjesto nula |
| Blok promijenjen od strane B | `READ_OK`, keš se osvježava | Direktno rješava mentorov scenario A/B |
| Prva replika nedostupna, druga radi | Petlja prelazi na sljedeću repliku, identično postojećem miss putu | Ponovna upotreba već testirane fallback logike |
| Sve replike nedostupne | Fail open na pun miss-fetch (§4, Opcija A) | Nikad tih pogrešan rezultat |
| `cache_max_bytes = 0` (keš isključen) | `cache_.get()` uvijek promašaj — ova grana se nikad ne izvršava | Nema promjene ponašanja za isključen keš, Eksperiment 3 OFF-red ostaje identičan |
| Konkurentni upis tačno između `get()` i slanja `READ_BLOCK`-a | Revalidacija i dalje ispravno vidi novu verziju (server-side provjera je atomska pod istim lock-om, `block_storage.cpp:431-436`, već dokumentovano od Faze 13) | Postojeća garancija se ne mijenja |

## 7. Uticaj na eksperimentalne rezultate — KLJUČNA razlika od Faze 18

Faza 18 (ABANDON_BLOCKS) ne mijenja nijedan izmjeren broj u Poglavlju 4 —
utiče samo na putanju otkaza koja se ne mjeri u postojećim eksperimentima.
**Ova izmjena mijenja mjereno ponašanje samog pogotka u kešu**, koje je
direktno izmjereno i objavljeno u tekstu:

- Apstrakt (oba jezika) i §Дискусија tvrde da keš smanjuje srednju latenciju
  čitanja "за фактор од приближно 4 до 8 пута" — ovaj broj je izmjeren pod
  **trenutnim** ponašanjem gdje pogodak ne pravi nijedan mrežni poziv.
- Eksperiment 3 (Кеш ON/OFF, `poglavlja/04_eksperimentalni_rezultati.tex`,
  tabela oko linije 246) sadrži mjerene min/mean/max i P95 latencije pogotka
  koje bi se, poslije ove izmjene, promijenile (pogodak dobija jedan
  mrežni round-trip umjesto nula) — brojevi bi i dalje pokazivali ubrzanje
  u odnosu na OFF (jer se ne prenose podaci, samo mala potvrda), ali NE
  istu apsolutnu vrijednost kao trenutno objavljena.
- Novododati pasus o "dugom repu latencije promašaja" (komentar #10,
  linija ~312) upoređuje mean vs. P95 baš na osnovu trenutnih ON brojeva —
  posredno bi zahtijevao ponovnu provjeru.

**Zaključak: za razliku od Faze 18, ovu izmjenu NIJE bezbjedno opisati u
tekstu kao "već implementirano" prije nego što se stvarno implementira I
Eksperiment 3 (barem) ponovo izmjeri.** Faza 18 je bila čisto strukturna
ispravka bez uticaja na postojeće brojeve; ova je direktno mjerena
performance karakteristika koja je već objavljena sa konkretnim ciframa u
apstraktu.

## 8. Testovi (TDD — pisati prije koda)

**8.1 `tests/unit/test_cache.cpp`** — provjeriti da `ClientCache` API
ostaje dovoljan bez izmjena (nema novih testova potrebnih za sam `Cache`,
pošto se logika revalidacije nalazi u `Client`, ne u `ClientCache`).

**8.2 novi test-fajl ili prošireni `tests/integration/test_read.cpp`**
(pravi multi-proces klaster, po uzoru na postojeće `failure-injection`
testove):
- `CacheHitRevalidatesAndDetectsChangeByOtherClient` — klijent A čita blok
  (keš miss, puni fetch), klijent B upisuje preko istog bloka, klijent A
  čita ponovo → mora dobiti B-ov novi sadržaj, ne stari keširani
- `CacheHitRevalidatesAndConfirmsUnchangedBlock` — dva uzastopna čitanja
  bez ikakve izmjene → drugo čitanje i dalje pogodak, ali provjeriti (npr.
  brojanjem poruka na test-mock konekciji ili statistikom na `Storage`
  strani) da je poslat `READ_BLOCK` sa nenultim `known_version` i da je
  odgovor `NOT_MODIFIED`
- `CacheRevalidationFailsOpenWhenAllReplicasUnreachable` — ugasiti sve
  replike bloka poslije prvog (uspješnog) čitanja, provjeriti da drugo
  čitanje ili uspije preko fallback miss-puta ili vrati `BLOCK_NOT_FOUND`,
  nikad zastarjele podatke bez signalizacije
- `DisabledCacheUnaffectedByRevalidationPath` (`cache_max_bytes=0`) —
  regresija da OFF-režim ostaje netaknut

**8.3 Ponovo pokrenuti Eksperiment 3** (`bench/`) poslije implementacije
i **ažurirati** tabelu i pasus o mean/P95 latenciji pogotka
(`poglavlja/04_eksperimentalni_rezultati.tex`) i abstraktni broj "4 do 8
puta" prije nego što se tekst smatra konačnim.

## 9. Kritični fajlovi

- `src/client/client.cpp` (`read()`, linije ~690-747) — jedina stvarna
  izmjena koda
- `tests/integration/test_read.cpp` (novi ili prošireni)
- `poglavlja/04_eksperimentalni_rezultati.tex` — Eksperiment 3 tabela i
  pasus o tail latenciji, MORAJU se ažurirati poslije implementacije
- `poglavlja/00b_apstrakt.tex` — "4 до 8 пута" broj, provjeriti poslije
  ponovnog mjerenja

## 10. Redoslijed implementacije (kad bude odobreno)

1. Testovi iz §8.2 (moraju pasti bez fixa).
2. Izmjena `Client::read()` (§3, §4).
3. Build + pun test suite (`tests/unit/`, `tests/integration/`,
   `tests/failure/`).
4. Ponovo pokrenuti Eksperiment 3 (loopback i, ako se radi puna VM
   revalidacija, i VM topologiju) i ažurirati brojeve u §7.
5. Tek nakon 4 — ažurirati tekst diplomskog rada da opisuje mehanizam
   (za razliku od Faze 18, ovdje se tekst piše POSLIJE mjerenja, ne prije).
