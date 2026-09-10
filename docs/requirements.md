# Requirements

Pojednostavljeni GFS-nalik distribuirani fajl sistem, razvijen kao
diplomski rad. Podržane operacije: CREATE, READ, WRITE, APPEND, DELETE,
LIST, STAT.

Ključni parametri sistema:

- Veličina bloka: 64 MB (`BLOCK_SIZE`)
- Replication factor: 3 (`DEFAULT_REPLICATION_FACTOR`)
- Detekcija otkaza: heartbeat interval 2s, failure timeout 6s

Cilj je funkcionalan, testiran sistem koji demonstrira ključne koncepte
distribuiranih fajl sistema (particionisanje na blokove, replikacija,
detekcija i oporavak od otkaza, odvajanje kontrolne i podatkovne ravni)
bez pretenzije na produkcijsku robusnost.
