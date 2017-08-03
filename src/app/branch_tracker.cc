// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/branch_tracker.h"

#include <vector>

#include "apps/ledger/src/app/diff_utils.h"
#include "apps/ledger/src/app/fidl/serialization_size.h"
#include "apps/ledger/src/app/page_manager.h"
#include "apps/ledger/src/app/page_utils.h"
#include "apps/ledger/src/callback/waiter.h"
#include "lib/ftl/functional/auto_call.h"
#include "lib/ftl/functional/make_copyable.h"

namespace ledger {
class BranchTracker::PageWatcherContainer {
 public:
  PageWatcherContainer(coroutine::CoroutineService* coroutine_service,
                       PageWatcherPtr watcher,
                       PageManager* page_manager,
                       storage::PageStorage* storage,
                       std::unique_ptr<const storage::Commit> base_commit,
                       std::string key_prefix)
      : change_in_flight_(false),
        last_commit_(std::move(base_commit)),
        coroutine_service_(coroutine_service),
        key_prefix_(std::move(key_prefix)),
        manager_(page_manager),
        storage_(storage),
        interface_(std::move(watcher)) {
    interface_.set_connection_error_handler([this] {
      if (handler_) {
        handler_->Continue(true);
      }
      if (on_empty_callback_) {
        on_empty_callback_();
      }
    });
  }

  ~PageWatcherContainer() {
    if (on_drained_) {
      on_drained_();
    }
    if (handler_) {
      handler_->Continue(true);
    }
    FTL_DCHECK(!handler_);
  }

  void set_on_empty(ftl::Closure on_empty_callback) {
    on_empty_callback_ = std::move(on_empty_callback);
  }

  void UpdateCommit(std::unique_ptr<const storage::Commit> commit) {
    current_commit_ = std::move(commit);
    SendCommit();
  }

  // Sets a callback to be called when all pending updates are sent. If all
  // updates are already sent, the callback will be called immediately. This
  // callback will only be called once; |SetOnDrainedCallback| should be called
  // again to set a new callback after the first one is called. Setting a
  // callback while a previous one is still active will execute the previous
  // callback.
  void SetOnDrainedCallback(ftl::Closure on_drained) {
    // If a transaction is committed or rolled back before all watchers have
    // been drained, we do not want to continue blocking until they drain. Thus,
    // we declare them drained right away and proceed.
    if (on_drained_) {
      on_drained_();
      on_drained_ = nullptr;
    }
    on_drained_ = on_drained;
    if (Drained() && on_drained_) {
      on_drained();
      on_drained_ = nullptr;
    }
  }

 private:
  // Returns true if all changes have been sent to the watcher client, false
  // otherwise.
  bool Drained() {
    return !current_commit_ ||
           last_commit_->GetId() == current_commit_->GetId();
  }

  std::vector<PageChangePtr> PaginateChanges(PageChangePtr change) {
    std::vector<PageChangePtr> changes;

    // These are initialized to valid values in the first run of the loop.
    size_t fidl_size = -1;
    size_t handle_count = -1;
    size_t timestamp = change->timestamp;
    auto entries = std::move(change->changes);
    auto deletions = std::move(change->deleted_keys);
    for (size_t i = 0, j = 0; i < entries.size() || j < deletions.size();) {
      bool add_entry =
          i < entries.size() && (j == deletions.size() ||
                                 convert::ExtendedStringView(entries[i]->key) <
                                     convert::ExtendedStringView(deletions[j]));
      size_t entry_size =
          add_entry ? fidl_serialization::GetEntrySize(entries[i]->key.size())
                    : fidl_serialization::GetByteArraySize(deletions[j].size());
      size_t entry_handle_count = add_entry ? 1 : 0;

      if (changes.empty() ||
          fidl_size + entry_size > fidl_serialization::kMaxInlineDataSize ||
          handle_count + entry_handle_count >
              fidl_serialization::kMaxMessageHandles) {
        changes.push_back(PageChange::New());
        changes.back()->timestamp = timestamp;
        changes.back()->changes = fidl::Array<EntryPtr>::New(0);
        changes.back()->deleted_keys =
            fidl::Array<fidl::Array<uint8_t>>::New(0);
        fidl_size = fidl_serialization::kPageChangeHeaderSize;
        handle_count = 0u;
      }
      fidl_size += entry_size;
      handle_count += entry_handle_count;
      if (add_entry) {
        changes.back()->changes.push_back(std::move(entries[i]));
        ++i;
      } else {
        changes.back()->deleted_keys.push_back(std::move(deletions[j]));
        ++j;
      }
    }
    return changes;
  }

  void SendChange(PageChangePtr page_change,
                  ResultState state,
                  std::unique_ptr<const storage::Commit> new_commit,
                  ftl::Closure on_done) {
    interface_->OnChange(
        std::move(page_change), state, ftl::MakeCopyable([
          this, state, new_commit = std::move(new_commit),
          on_done = std::move(on_done)
        ](fidl::InterfaceRequest<PageSnapshot> snapshot_request) mutable {
          if (snapshot_request) {
            manager_->BindPageSnapshot(
                new_commit->Clone(), std::move(snapshot_request), key_prefix_);
          }
          if (state != ResultState::COMPLETED &&
              state != ResultState::PARTIAL_COMPLETED) {
            on_done();
            return;
          }
          change_in_flight_ = false;
          last_commit_.swap(new_commit);
          // SendCommit will start handling the following commit, so we need to
          // make sure on_done() is called before that.
          on_done();
          SendCommit();
        }));
  }

  // Sends a commit to the watcher if needed.
  void SendCommit() {
    if (change_in_flight_) {
      return;
    }

    if (Drained()) {
      if (on_drained_) {
        on_drained_();
        on_drained_ = nullptr;
      }
      return;
    }

    change_in_flight_ = true;

    // TODO(etiennej): See LE-74: clean object ownership
    diff_utils::ComputePageChange(
        storage_, *last_commit_, *current_commit_, key_prefix_, key_prefix_,
        diff_utils::PaginationBehavior::NO_PAGINATION,
        ftl::MakeCopyable([ this, new_commit = std::move(current_commit_) ](
            Status status,
            std::pair<PageChangePtr, std::string> page_change_ptr) mutable {
          if (status != Status::OK) {
            // This change notification is abandonned. At the next commit,
            // we will try again (but not before). The next notification
            // will cover both this change and the next.
            FTL_LOG(ERROR) << "Unable to compute PageChange for Watch update.";
            change_in_flight_ = false;
            return;
          }

          if (!page_change_ptr.first) {
            change_in_flight_ = false;
            last_commit_.swap(new_commit);
            SendCommit();
            return;
          }
          std::vector<PageChangePtr> paginated_changes =
              PaginateChanges(std::move(page_change_ptr.first));
          if (paginated_changes.size() == 1) {
            SendChange(std::move(paginated_changes[0]), ResultState::COMPLETED,
                       std::move(new_commit), [] {});
            return;
          }
          coroutine_service_->StartCoroutine(ftl::MakeCopyable([
            this, new_commit = std::move(new_commit),
            paginated_changes = std::move(paginated_changes)
          ](coroutine::CoroutineHandler * handler) mutable {
            auto guard = ftl::MakeAutoCall([this] { handler_ = nullptr; });
            FTL_DCHECK(!handler_);
            handler_ = handler;
            for (size_t i = 0; i < paginated_changes.size(); ++i) {
              ResultState state;
              if (i == 0) {
                state = ResultState::PARTIAL_STARTED;
              } else if (i == paginated_changes.size() - 1) {
                state = ResultState::PARTIAL_COMPLETED;
              } else {
                state = ResultState::PARTIAL_CONTINUED;
              }
              if (coroutine::SyncCall(
                      handler, ftl::MakeCopyable([
                        this, change = std::move(paginated_changes[i]), state,
                        new_commit = new_commit->Clone()
                      ](ftl::Closure on_done) mutable {
                        SendChange(std::move(change), state,
                                   std::move(new_commit), std::move(on_done));

                      }))) {
                return;
              }
            }
          }));
        }));
  }

  ftl::Closure on_drained_ = nullptr;
  ftl::Closure on_empty_callback_ = nullptr;
  bool change_in_flight_;
  std::unique_ptr<const storage::Commit> last_commit_;
  std::unique_ptr<const storage::Commit> current_commit_;
  coroutine::CoroutineService* coroutine_service_;
  coroutine::CoroutineHandler* handler_ = nullptr;
  const std::string key_prefix_;
  PageManager* manager_;
  storage::PageStorage* storage_;
  PageWatcherPtr interface_;
};

BranchTracker::BranchTracker(coroutine::CoroutineService* coroutine_service,
                             PageManager* manager,
                             storage::PageStorage* storage)
    : coroutine_service_(coroutine_service),
      manager_(manager),
      storage_(storage),
      transaction_in_progress_(false),
      current_commit_(nullptr),
      weak_factory_(this) {
  watchers_.set_on_empty([this] { CheckEmpty(); });
}

BranchTracker::~BranchTracker() {
  storage_->RemoveCommitWatcher(this);
}

void BranchTracker::Init(std::function<void(Status)> on_done) {
  storage_->GetHeadCommitIds([
    weak_this = weak_factory_.GetWeakPtr(), on_done = std::move(on_done)
  ](storage::Status status, std::vector<storage::CommitId> commit_ids) {
    if (!weak_this) {
      return;
    }
    if (status != storage::Status::OK) {
      on_done(PageUtils::ConvertStatus(status));
      return;
    }

    FTL_DCHECK(!commit_ids.empty());
    weak_this->InitCommitAndSetWatcher(std::move(commit_ids[0]));

    on_done(Status::OK);
  });
}

void BranchTracker::set_on_empty(ftl::Closure on_empty_callback) {
  on_empty_callback_ = on_empty_callback;
}

const storage::CommitId& BranchTracker::GetBranchHeadId() {
  return current_commit_id_;
}

void BranchTracker::OnNewCommits(
    const std::vector<std::unique_ptr<const storage::Commit>>& commits,
    storage::ChangeSource /*source*/) {
  bool changed = false;
  const std::unique_ptr<const storage::Commit>* new_current_commit = nullptr;
  for (const auto& commit : commits) {
    if (commit->GetId() == current_commit_id_) {
      continue;
    }
    // This assumes commits are received in (partial) order. If the commit
    // doesn't have current_commit_id_ as a parent it is not part of this branch
    // and should be ignored.
    std::vector<storage::CommitIdView> parent_ids = commit->GetParentIds();
    if (std::find(parent_ids.begin(), parent_ids.end(), current_commit_id_) ==
        parent_ids.end()) {
      continue;
    }
    changed = true;
    current_commit_id_ = commit->GetId();
    new_current_commit = &commit;
  }
  if (changed) {
    current_commit_ = (*new_current_commit)->Clone();
  }

  if (!changed || transaction_in_progress_) {
    return;
  }
  for (auto& watcher : watchers_) {
    watcher.UpdateCommit(current_commit_->Clone());
  }
}

void BranchTracker::StartTransaction(ftl::Closure watchers_drained_callback) {
  FTL_DCHECK(!transaction_in_progress_);
  transaction_in_progress_ = true;
  auto waiter = callback::CompletionWaiter::Create();
  for (auto& watcher : watchers_) {
    watcher.SetOnDrainedCallback(waiter->NewCallback());
  }
  waiter->Finalize(std::move(watchers_drained_callback));
}

void BranchTracker::StopTransaction(
    std::unique_ptr<const storage::Commit> commit) {
  FTL_DCHECK(transaction_in_progress_ || !commit);

  if (!transaction_in_progress_) {
    return;
  }
  transaction_in_progress_ = false;

  if (commit) {
    current_commit_id_ = commit->GetId();
    current_commit_ = std::move(commit);
  }

  if (!current_commit_) {
    // current_commit_ has a null value only if OnNewCommits has neven been
    // called. Here we are in the case where a transaction stops, but no new
    // commits have arrived in between: there is no need to update the watchers.
    return;
  }

  for (auto& watcher : watchers_) {
    watcher.SetOnDrainedCallback(nullptr);
    watcher.UpdateCommit(current_commit_->Clone());
  }
}

void BranchTracker::RegisterPageWatcher(
    PageWatcherPtr page_watcher_ptr,
    std::unique_ptr<const storage::Commit> base_commit,
    std::string key_prefix) {
  watchers_.emplace(coroutine_service_, std::move(page_watcher_ptr), manager_,
                    storage_, std::move(base_commit), std::move(key_prefix));
}

bool BranchTracker::IsEmpty() {
  return watchers_.empty();
}

void BranchTracker::InitCommitAndSetWatcher(storage::CommitId commit_id) {
  // current_commit_ will be updated to have a correct value after the first
  // Commit received in OnNewCommits or StopTransaction.
  FTL_DCHECK(!current_commit_);
  current_commit_id_ = std::move(commit_id);
  storage_->AddCommitWatcher(this);
}

void BranchTracker::CheckEmpty() {
  if (on_empty_callback_ && IsEmpty())
    on_empty_callback_();
}

}  // namespace ledger
