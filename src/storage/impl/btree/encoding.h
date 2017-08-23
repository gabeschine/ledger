// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_STORAGE_IMPL_BTREE_ENCODING_H_
#define APPS_LEDGER_SRC_STORAGE_IMPL_BTREE_ENCODING_H_

#include <string>

#include "apps/ledger/src/storage/public/types.h"
#include "lib/ftl/strings/string_view.h"

namespace storage {
namespace btree {

bool CheckValidTreeNodeSerialization(ftl::StringView data);

std::string EncodeNode(uint8_t level,
                       const std::vector<Entry>& entries,
                       const std::vector<ObjectId>& children);

bool DecodeNode(ftl::StringView data,
                uint8_t* level,
                std::vector<Entry>* res_entries,
                std::vector<ObjectId>* res_children);

}  // namespace btree
}  // namespace storage

#endif  // APPS_LEDGER_SRC_STORAGE_IMPL_BTREE_ENCODING_H_
