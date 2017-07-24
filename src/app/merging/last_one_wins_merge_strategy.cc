// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/merging/last_one_wins_merge_strategy.h"

#include <memory>
#include <string>

#include "apps/ledger/src/app/page_manager.h"
#include "apps/ledger/src/app/page_utils.h"
#include "lib/ftl/functional/closure.h"
#include "lib/ftl/memory/weak_ptr.h"

namespace ledger {

class LastOneWinsMergeStrategy::LastOneWinsMerger {
 public:
  LastOneWinsMerger(storage::PageStorage* storage,
                    std::unique_ptr<const storage::Commit> left,
                    std::unique_ptr<const storage::Commit> right,
                    std::unique_ptr<const storage::Commit> ancestor,
                    std::function<void(Status)> callback);
  ~LastOneWinsMerger();

  void Start();
  void Cancel();

 private:
  void Done(Status status);

  storage::PageStorage* const storage_;

  std::unique_ptr<const storage::Commit> const left_;
  std::unique_ptr<const storage::Commit> const right_;
  std::unique_ptr<const storage::Commit> const ancestor_;

  std::function<void(Status)> callback_;

  std::unique_ptr<storage::Journal> journal_;
  bool cancelled_ = false;

  // This must be the last member of the class.
  ftl::WeakPtrFactory<LastOneWinsMerger> weak_factory_;
};

LastOneWinsMergeStrategy::LastOneWinsMerger::LastOneWinsMerger(
    storage::PageStorage* storage,
    std::unique_ptr<const storage::Commit> left,
    std::unique_ptr<const storage::Commit> right,
    std::unique_ptr<const storage::Commit> ancestor,
    std::function<void(Status)> callback)
    : storage_(storage),
      left_(std::move(left)),
      right_(std::move(right)),
      ancestor_(std::move(ancestor)),
      callback_(std::move(callback)),
      weak_factory_(this) {
  FTL_DCHECK(callback_);
}

LastOneWinsMergeStrategy::LastOneWinsMerger::~LastOneWinsMerger() {
  if (journal_) {
    storage_->RollbackJournal(std::move(journal_));
  }
}

void LastOneWinsMergeStrategy::LastOneWinsMerger::Start() {
  storage::Status s =
      storage_->StartMergeCommit(left_->GetId(), right_->GetId(), &journal_);
  FTL_DCHECK(s == storage::Status::OK);

  auto on_next = [weak_this =
                      weak_factory_.GetWeakPtr()](storage::EntryChange change) {
    if (!weak_this || weak_this->cancelled_) {
      // No need to call Done, as it will be called in the on_done callback.
      return false;
    }
    const std::string& key = change.entry.key;
    storage::Status s;
    if (change.deleted) {
      s = weak_this->journal_->Delete(key);
    } else {
      s = weak_this->journal_->Put(key, change.entry.object_id,
                                   change.entry.priority);
    }
    if (s != storage::Status::OK) {
      FTL_LOG(ERROR) << "Error while merging commits: " << s;
    }
    return true;
  };

  auto on_diff_done = [weak_this =
                           weak_factory_.GetWeakPtr()](storage::Status s) {
    if (!weak_this) {
      return;
    }
    if (weak_this->cancelled_) {
      weak_this->Done(Status::INTERNAL_ERROR);
      return;
    }
    if (s != storage::Status::OK) {
      FTL_LOG(ERROR) << "Unable to create diff for merging: " << s;
      weak_this->Done(PageUtils::ConvertStatus(s));
      return;
    }
    weak_this->storage_->CommitJournal(
        std::move(weak_this->journal_),
        [weak_this](storage::Status s, std::unique_ptr<const storage::Commit>) {
          if (s != storage::Status::OK) {
            FTL_LOG(ERROR) << "Unable to commit merge journal: " << s;
          }
          if (weak_this) {
            weak_this->Done(
                PageUtils::ConvertStatus(s, Status::INTERNAL_ERROR));
          }
        });
  };
  storage_->GetCommitContentsDiff(*ancestor_, *right_, "", std::move(on_next),
                                  std::move(on_diff_done));
}

void LastOneWinsMergeStrategy::LastOneWinsMerger::Cancel() {
  cancelled_ = true;
  if (journal_) {
    storage_->RollbackJournal(std::move(journal_));
    journal_.reset();
  }
}

void LastOneWinsMergeStrategy::LastOneWinsMerger::Done(Status status) {
  auto callback = std::move(callback_);
  callback_ = nullptr;
  callback(status);
}

LastOneWinsMergeStrategy::LastOneWinsMergeStrategy() {}

LastOneWinsMergeStrategy::~LastOneWinsMergeStrategy() {}

void LastOneWinsMergeStrategy::SetOnError(std::function<void()> on_error) {}

void LastOneWinsMergeStrategy::Merge(
    storage::PageStorage* storage,
    PageManager* page_manager,
    std::unique_ptr<const storage::Commit> head_1,
    std::unique_ptr<const storage::Commit> head_2,
    std::unique_ptr<const storage::Commit> ancestor,
    std::function<void(Status)> callback) {
  FTL_DCHECK(!in_progress_merge_);
  FTL_DCHECK(head_1->GetTimestamp() <= head_2->GetTimestamp());

  in_progress_merge_ =
      std::make_unique<LastOneWinsMergeStrategy::LastOneWinsMerger>(
          storage, std::move(head_1), std::move(head_2), std::move(ancestor),
          [ this, callback = std::move(callback) ](Status status) {
            in_progress_merge_.reset();
            callback(status);
          });

  in_progress_merge_->Start();
}

void LastOneWinsMergeStrategy::Cancel() {
  if (in_progress_merge_) {
    in_progress_merge_->Cancel();
  }
}

}  // namespace ledger
