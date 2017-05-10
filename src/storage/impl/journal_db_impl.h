// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_STORAGE_IMPL_JOURNAL_DB_IMPL_H_
#define APPS_LEDGER_SRC_STORAGE_IMPL_JOURNAL_DB_IMPL_H_

#include "apps/ledger/src/storage/public/journal.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

#include "apps/ledger/src/coroutine/coroutine.h"
#include "apps/ledger/src/storage/impl/db.h"
#include "apps/ledger/src/storage/impl/page_storage_impl.h"
#include "apps/ledger/src/storage/public/commit.h"
#include "apps/ledger/src/storage/public/types.h"
#include "lib/ftl/macros.h"

namespace storage {

// A |JournalDBImpl| represents a commit in progress.
class JournalDBImpl : public Journal {
 public:
  ~JournalDBImpl() override;

  // Creates a new Journal for a simple commit.
  static std::unique_ptr<Journal> Simple(
      JournalType type,
      coroutine::CoroutineService* coroutine_service,
      PageStorageImpl* page_storage,
      DB* db,
      const JournalId& id,
      const CommitId& base);

  // Creates a new Journal for a merge commit.
  static std::unique_ptr<Journal> Merge(
      coroutine::CoroutineService* coroutine_service,
      PageStorageImpl* page_storage,
      DB* db,
      const JournalId& id,
      const CommitId& base,
      const CommitId& other);

  // Returns the id of this journal.
  const JournalId& GetId() const;

  // Journal :
  Status Put(convert::ExtendedStringView key,
             ObjectIdView object_id,
             KeyPriority priority) override;
  Status Delete(convert::ExtendedStringView key) override;
  void Commit(
      std::function<void(Status, std::unique_ptr<const storage::Commit>)>
          callback) override;
  Status Rollback() override;

 private:
  JournalDBImpl(JournalType type,
                coroutine::CoroutineService* coroutine_service,
                PageStorageImpl* page_storage,
                DB* db,
                const JournalId& id,
                const CommitId& base);

  Status UpdateValueCounter(ObjectIdView object_id,
                            const std::function<int64_t(int64_t)>& operation);

  void GetParents(
      std::function<void(Status,
                         std::vector<std::unique_ptr<const storage::Commit>>)>
          callback);

  Status ClearCommittedJournal(std::unordered_set<ObjectId> new_nodes);

  const JournalType type_;
  coroutine::CoroutineService* const coroutine_service_;
  PageStorageImpl* const page_storage_;
  DB* const db_;
  const JournalId id_;
  CommitId base_;
  std::unique_ptr<CommitId> other_;
  // A journal is no longer valid if either commit or rollback have been
  // executed.
  bool valid_;
  // |failed_operation_| is true if any of the Put or Delete methods in this
  // journal have failed. In this case, any operation on EXPLICIT journals
  // other than rolling back will fail. IMPLICIT journals can still be commited
  // even if some operations have failed.
  bool failed_operation_;
};

}  // namespace storage

#endif  // APPS_LEDGER_SRC_STORAGE_IMPL_JOURNAL_DB_IMPL_H_
