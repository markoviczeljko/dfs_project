#ifndef DFS_MASTER_METADATA_H_
#define DFS_MASTER_METADATA_H_

#include <cstdint>
#include <string>
#include <vector>

#include "common/types.h"

struct FileMetadata {
    FileId id;
    std::string path;
    uint64_t size;
    uint64_t modification_time;
    std::vector<BlockId> blocks;
};

// Master-side recovery bookkeeping state (transitions implemented in the
// Failure Handling phase) — defined here now so the type exists wherever
// BlockMetadata is used.
enum class BlockState {
    ALLOCATED,
    WRITING,
    COMMITTED,
    UNDER_REPLICATED,
    RECOVERING,
    CORRUPTED,
    DELETING
};

struct BlockMetadata {
    BlockId id;
    uint64_t size;            // Confirmed size — changes only on a successful commit.
    uint64_t reserved_size;   // Faza 10: monotonically non-decreasing append-offset
                               // cursor; >= size. See docs/design.md §14.
    uint64_t version;
    std::vector<StorageNodeId> replicas;
    BlockState state;
};

// Faza 10: what Master tells a Client to do about the tail of a file's
// current last block before it appends any brand-new blocks. has_target ==
// false means the file has no blocks yet, or its last block is already
// full — either way the Client falls through to allocating fresh blocks
// exactly like a normal write.
// Faza 12.3: the version the target block will have once this append's
// fill commits (current version + 1) — Client relays it back to
// StorageNode's WRITE_BLOCK unchanged, since only Master ever advances a
// block's version (see docs/design.md §16). Meaningless when has_target is
// false.
struct AppendTarget {
    bool has_target;
    BlockId block_id;
    uint64_t offset;
    uint32_t fill_amount;
    std::vector<StorageNodeId> replicas;
    uint64_t target_version;
};

// Faza 10: everything NamespaceManager::commit_append() needs. A plain
// struct rather than positional parameters — several fields share a type
// (two uint64_t "prior size" values, a uint32_t count next to a uint64_t
// count), which made call sites easy to transpose by accident during
// development (see docs/design.md §14).
struct AppendCommitRequest {
    bool has_existing_target;
    BlockId existing_block_id;
    uint64_t expected_prior_size;
    uint32_t filled_amount;
    std::vector<BlockId> new_block_ids;
    uint64_t last_new_block_size;  // ignored when new_block_ids is empty
    uint64_t total_appended_bytes;
    // Faza 12.3: the value commit_append() stamps onto the existing
    // block's version on a successful CAS — echoed back from the
    // AppendTarget this commit is finalizing (see docs/design.md §16).
    // Meaningless when has_existing_target is false. Appended as the LAST
    // field (rather than grouped with the other has_existing_target-only
    // fields above) specifically so every existing positional
    // AppendCommitRequest{...} initializer in tests/master.cpp keeps
    // compiling unchanged — this is the only new field, so a single
    // trailing default keeps every call site's field order intact.
    uint64_t expected_target_version = 0;
};

// Faza 11: one block Master is dropping from its own bookkeeping — either
// because a file was DELETEd, or because a WRITE overwrote it (the §13.2
// kind of "orphan" — NOT the §8.34 kind, an unknown-to-Master block a
// Storage Node reports, which needs Faza 12's inventory/heartbeat
// reconciliation and is out of scope here). Carries a snapshot of the
// replicas from BlockMetadata at the moment of removal, since the
// BlockMetadata entry itself is erased in the same NamespaceManager call
// that produces this struct — Master needs the replica list afterward to
// dispatch DELETE_BLOCK. See docs/design.md §15.
struct OrphanedBlock {
    BlockId id;
    std::vector<StorageNodeId> replicas;
};

#endif  // DFS_MASTER_METADATA_H_
