// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_STORAGE_IMPL_COMMIT_RANDOM_IMPL_H_
#define APPS_LEDGER_SRC_STORAGE_IMPL_COMMIT_RANDOM_IMPL_H_

#include <memory>
#include <string>
#include <vector>

#include "apps/ledger/src/storage/public/commit.h"

namespace storage {
namespace test {

// Implementaton of Commit returning random values (fixed for each instance).
class CommitRandomImpl : public Commit {
 public:
  CommitRandomImpl();
  ~CommitRandomImpl() override = default;

  // Commit:
  std::unique_ptr<Commit> Clone() const override;

  const CommitId& GetId() const override;

  std::vector<CommitIdView> GetParentIds() const override;

  int64_t GetTimestamp() const override;

  uint64_t GetGeneration() const override;

  ObjectIdView GetRootId() const override;

  ftl::StringView GetStorageBytes() const override;

 private:
  CommitRandomImpl(const CommitRandomImpl& other);

  CommitId id_;
  int64_t timestamp_;
  uint64_t generation_;
  ObjectId root_node_id_;
  std::vector<CommitId> parent_ids_;
  std::vector<CommitIdView> parent_ids_views_;
  std::string storage_bytes_;
};

}  // namespace test
}  // namespace storage

#endif  // APPS_LEDGER_SRC_STORAGE_IMPL_COMMIT_RANDOM_IMPL_H_
