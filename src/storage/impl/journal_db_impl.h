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
#include "apps/ledger/src/storage/impl/page_db.h"
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
      PageDb* db,
      const JournalId& id,
      const CommitId& base);

  // Creates a new Journal for a merge commit.
  static std::unique_ptr<Journal> Merge(
      coroutine::CoroutineService* coroutine_service,
      PageStorageImpl* page_storage,
      PageDb* db,
      const JournalId& id,
      const CommitId& base,
      const CommitId& other);

  // Returns the id of this journal.
  const JournalId& GetId() const;

  // Commits the changes of this |Journal|. Trying to update entries or rollback
  // will fail after a successful commit. The callback will be called with the
  // returned status and the new commit.
  void Commit(
      std::function<void(Status, std::unique_ptr<const storage::Commit>)>
          callback);

  // Rolls back all changes to this |Journal|. Trying to update entries or
  // commit will fail with an |ILLEGAL_STATE| after a successful rollback.
  Status Rollback();

  // Journal:
  Status Put(convert::ExtendedStringView key,
             ObjectIdView object_id,
             KeyPriority priority) override;
  Status Delete(convert::ExtendedStringView key) override;

 private:
  JournalDBImpl(JournalType type,
                coroutine::CoroutineService* coroutine_service,
                PageStorageImpl* page_storage,
                PageDb* db,
                JournalId id,
                CommitId base);

  void GetParents(
      std::function<void(Status,
                         std::vector<std::unique_ptr<const storage::Commit>>)>
          callback);

  Status GetObjectsToSync(std::vector<ObjectId>* objects_to_sync);

  const JournalType type_;
  coroutine::CoroutineService* const coroutine_service_;
  PageStorageImpl* const page_storage_;
  PageDb* const db_;
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
