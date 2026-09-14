# Architecture
Posljednja sesija:  /home/zeljko/.claude/session-data/2026-09-12-dfs-phase1-network-session.tmp

Sistem se sastoji od tri tipa procesa:

- **Master** — kontrolna ravan: namespace, metadata fajlova, raspoređivanje
  blokova, koordinacija replikacije, detekcija otkaza Storage Node-ova.
  Ne učestvuje u prenosu podataka.
- **Storage Node** — podatkovna ravan: fizičko skladištenje blokova na
  disku, servisira READ/WRITE zahteve direktno od klijenta.
- **Client** — izlaže CREATE/READ/WRITE/APPEND/DELETE/LIST/STAT API,
  komunicira sa Master-om za metadata operacije, a sa Storage Node-ovima
  direktno za prenos podataka. Sadrži LRU keš blokova.

Ključni arhitektonski princip: strogo razdvajanje kontrolne ravni
(Master, metadata) od podatkovne ravni (Storage Node, sirovi bajtovi) —
Master nikad ne prenosi podatke fajlova.

Namespace je hijerarhijski (kao pravi fajl sistem), ne flat.

Detalji mrežnog protokola: `docs/network_protocol.md`.
