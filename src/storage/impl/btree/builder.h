// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_STORAGE_IMPL_BTREE_BUILDER_H_
#define APPS_LEDGER_SRC_STORAGE_IMPL_BTREE_BUILDER_H_

#include <memory>
#include <unordered_set>

#include "apps/ledger/src/coroutine/coroutine.h"
#include "apps/ledger/src/storage/public/iterator.h"
#include "apps/ledger/src/storage/public/page_storage.h"
#include "apps/ledger/src/storage/public/types.h"

namespace storage {
namespace btree {

struct NodeLevelCalculator {
  // Returns the level in the tree where a node containing |key| must be
  // located. The leaves are located on level 0.
  uint8_t (*GetNodeLevel)(convert::ExtendedStringView key);
};

// Returns the default algorithm to compute the node level.
const NodeLevelCalculator* GetDefaultNodeLevelCalculator();

// Applies changes provided by |changes| to the B-Tree starting at |root_id|.
// |changes| must provide |EntryChange| objects sorted by their key. The
// callback will provide the status of the operation, the id of the new root
// and the list of ids of all new nodes created after the changes.
void ApplyChanges(
    coroutine::CoroutineService* coroutine_service,
    PageStorage* page_storage,
    ObjectIdView root_id,
    std::unique_ptr<Iterator<const EntryChange>> changes,
    std::function<void(Status, ObjectId, std::unordered_set<ObjectId>)>
        callback,
    const NodeLevelCalculator* node_level_calculator =
        GetDefaultNodeLevelCalculator());

}  // namespace btree
}  // namespace storage

#endif  // APPS_LEDGER_SRC_STORAGE_IMPL_BTREE_BUILDER_H_
