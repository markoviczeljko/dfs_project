# Network Protocol

Custom binarni protokol preko TCP-a, korišćen za komunikaciju
Client↔Master (kontrolne poruke) i Client↔Storage Node (podatkovne
poruke).

Greške se signaliziraju kroz `ErrorCode` (vidi `include/common/errors.h`):
`FILE_NOT_FOUND`, `FILE_ALREADY_EXISTS`, `BLOCK_NOT_FOUND`,
`NODE_NOT_FOUND`, `BLOCK_CORRUPTED`, `STORAGE_FULL`, `TIMEOUT`,
`INTERNAL_ERROR`, uz `INVALID_REQUEST`/`INVALID_MESSAGE` za malformisane
poruke.

Detaljna specifikacija formata poruka (header layout, serijalizacija,
tipovi poruka po operaciji) implementira se u Fazi 1, u `include/network/`.
