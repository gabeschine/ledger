// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/merging/custom_merge_strategy.h"

#include <memory>

#include "apps/ledger/src/app/page_manager.h"
#include "lib/ftl/functional/closure.h"

namespace ledger {
CustomMergeStrategy::CustomMergeStrategy(ConflictResolverPtr conflict_resolver)
    : conflict_resolver_(std::move(conflict_resolver)) {
  conflict_resolver_.set_connection_error_handler([this]() {
    // If a merge is in progress, it must be terminated.
    if (in_progress_merge_) {
      // The actual cleanup of in_progress_merge_ will happen in its on_done
      // callback.
      in_progress_merge_->Cancel();
    }
    if (on_error_) {
      // It is safe to call |on_error_| because the error handler waits for the
      // merges to finish before deleting this object.
      on_error_();
    }
  });
}

CustomMergeStrategy::~CustomMergeStrategy() {}

void CustomMergeStrategy::SetOnError(ftl::Closure on_error) {
  on_error_ = on_error;
}

void CustomMergeStrategy::Merge(storage::PageStorage* storage,
                                PageManager* page_manager,
                                std::unique_ptr<const storage::Commit> head_1,
                                std::unique_ptr<const storage::Commit> head_2,
                                std::unique_ptr<const storage::Commit> ancestor,
                                std::function<void(Status)> callback) {
  FTL_DCHECK(head_1->GetTimestamp() <= head_2->GetTimestamp());
  FTL_DCHECK(!in_progress_merge_);

  in_progress_merge_ = std::make_unique<ConflictResolverClient>(
      storage, page_manager, conflict_resolver_.get(), std::move(head_2),
      std::move(head_1), std::move(ancestor),
      [ this, callback = std::move(callback) ](Status status) {
        in_progress_merge_.reset();
        callback(status);
      });

  in_progress_merge_->Start();
}

void CustomMergeStrategy::Cancel() {
  if (in_progress_merge_) {
    in_progress_merge_->Cancel();
  }
}

}  // namespace ledger
