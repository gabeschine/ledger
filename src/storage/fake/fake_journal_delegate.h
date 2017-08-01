// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_STORAGE_FAKE_FAKE_JOURNAL_DELEGATE_H_
#define APPS_LEDGER_SRC_STORAGE_FAKE_FAKE_JOURNAL_DELEGATE_H_

#include <map>
#include <string>

#include "apps/ledger/src/convert/convert.h"
#include "apps/ledger/src/storage/public/commit.h"
#include "apps/ledger/src/storage/public/types.h"
#include "lib/ftl/macros.h"

namespace storage {
namespace fake {

// |FakeJournalDelegate| records the changes made through a journal. This
// object is owned by |FakePageStorage| and outlives |FakeJournal|.
class FakeJournalDelegate {
 public:
  struct Entry {
    ObjectId value;
    bool deleted;
    KeyPriority priority;
  };

  FakeJournalDelegate(CommitId parent_id, bool autocommit);
  ~FakeJournalDelegate();

  const CommitId& GetId() const { return id_; }

  Status SetValue(convert::ExtendedStringView key,
                  ObjectIdView value,
                  KeyPriority priority);
  Status Delete(convert::ExtendedStringView key);

  void Commit(
      std::function<void(Status, std::unique_ptr<const storage::Commit>)>
          callback);
  bool IsCommitted() const;

  Status Rollback();
  bool IsRolledBack() const;

  const CommitId& GetParentId() const { return parent_id_; }

  bool IsPendingCommit();
  void ResolvePendingCommit(Status status);

  const std::map<std::string, Entry, convert::StringViewComparator>& GetData()
      const;

 private:
  Entry& Get(convert::ExtendedStringView key);

  bool autocommit_;

  const CommitId id_;
  const CommitId parent_id_;
  std::map<std::string, Entry, convert::StringViewComparator> data_;

  bool is_committed_ = false;
  bool is_rolled_back_ = false;
  std::function<void(Status, std::unique_ptr<const storage::Commit>)>
      commit_callback_;

  FTL_DISALLOW_COPY_AND_ASSIGN(FakeJournalDelegate);
};

}  // namespace fake
}  // namespace storage

#endif  // APPS_LEDGER_SRC_STORAGE_FAKE_FAKE_JOURNAL_DELEGATE_H_
