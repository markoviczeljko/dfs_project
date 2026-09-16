#ifndef DFS_STORAGE_RECOVERY_H_
#define DFS_STORAGE_RECOVERY_H_

#include <cstddef>
#include <vector>

#include "common/types.h"
#include "storage/block_manager.h"

// Faza 12.3: builds a NODE_INVENTORY report payload from a Storage Node's
// actual on-disk block contents (see docs/design.md §16). A pure payload
// builder, not a class — same convention as block_header.h's free
// functions (see docs/design.md §9).
namespace StorageRecovery {

std::vector<std::byte> build_inventory_payload(StorageNodeId node_id, BlockManager& manager);

}  // namespace StorageRecovery

#endif  // DFS_STORAGE_RECOVERY_H_
