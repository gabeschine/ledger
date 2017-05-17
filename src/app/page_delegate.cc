// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/page_delegate.h"

#include <string>
#include <utility>
#include <vector>

#include "apps/ledger/src/app/constants.h"
#include "apps/ledger/src/app/page_manager.h"
#include "apps/ledger/src/app/page_snapshot_impl.h"
#include "apps/ledger/src/app/page_utils.h"
#include "apps/ledger/src/callback/waiter.h"
#include "apps/ledger/src/convert/convert.h"
#include "apps/tracing/lib/trace/event.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/mtl/socket/strings.h"

namespace ledger {

PageDelegate::PageDelegate(coroutine::CoroutineService* coroutine_service,
                           PageManager* manager,
                           storage::PageStorage* storage,
                           fidl::InterfaceRequest<Page> request)
    : manager_(manager),
      storage_(storage),
      interface_(std::move(request), this),
      branch_tracker_(coroutine_service, manager, storage) {
  interface_.set_on_empty([this] {
    operation_serializer_.Serialize(
        [](Status status) {},
        [this](std::function<void(Status)> callback) {
          branch_tracker_.StopTransaction(nullptr);
          callback(Status::OK);
        });
  });
  branch_tracker_.set_on_empty([this] { CheckEmpty(); });
  operation_serializer_.set_on_empty([this] { CheckEmpty(); });
}

PageDelegate::~PageDelegate() {}

// GetId() => (array<uint8> id);
void PageDelegate::GetId(const Page::GetIdCallback& callback) {
  callback(convert::ToArray(storage_->GetId()));
}

// GetSnapshot(PageSnapshot& snapshot, PageWatcher& watcher) => (Status status);
void PageDelegate::GetSnapshot(
    fidl::InterfaceRequest<PageSnapshot> snapshot_request,
    fidl::Array<uint8_t> key_prefix,
    fidl::InterfaceHandle<PageWatcher> watcher,
    const Page::GetSnapshotCallback& callback) {
  // TODO(qsr): Update this so that only |GetCurrentCommitId| is done in a the
  // operation serializer.
  operation_serializer_.Serialize(
      std::move(callback), ftl::MakeCopyable([
        this, snapshot_request = std::move(snapshot_request),
        key_prefix = std::move(key_prefix), watcher = std::move(watcher)
      ](Page::GetSnapshotCallback callback) mutable {
        storage_->GetCommit(
            GetCurrentCommitId(),
            ftl::MakeCopyable([
              this, snapshot_request = std::move(snapshot_request),
              key_prefix = std::move(key_prefix), watcher = std::move(watcher),
              callback = std::move(callback)
            ](storage::Status status,
              std::unique_ptr<const storage::Commit> commit) mutable {
              if (status != storage::Status::OK) {
                callback(PageUtils::ConvertStatus(status));
                return;
              }
              std::string prefix = convert::ToString(key_prefix);
              if (watcher) {
                PageWatcherPtr watcher_ptr =
                    PageWatcherPtr::Create(std::move(watcher));
                branch_tracker_.RegisterPageWatcher(std::move(watcher_ptr),
                                                    commit->Clone(), prefix);
              }
              manager_->BindPageSnapshot(std::move(commit),
                                         std::move(snapshot_request),
                                         std::move(prefix));
              callback(Status::OK);
            }));
      }));
}

// Put(array<uint8> key, array<uint8> value) => (Status status);
void PageDelegate::Put(fidl::Array<uint8_t> key,
                       fidl::Array<uint8_t> value,
                       const Page::PutCallback& callback) {
  PutWithPriority(std::move(key), std::move(value), Priority::EAGER,
                  std::move(callback));
}

// PutWithPriority(array<uint8> key, array<uint8> value, Priority priority)
//   => (Status status);
void PageDelegate::PutWithPriority(
    fidl::Array<uint8_t> key,
    fidl::Array<uint8_t> value,
    Priority priority,
    const Page::PutWithPriorityCallback& callback) {
  auto promise = callback::Promise<storage::Status, storage::ObjectId>::Create(
      storage::Status::ILLEGAL_STATE);
  storage_->AddObjectFromLocal(storage::DataSource::Create(std::move(value)),
                               promise->NewCallback());

  operation_serializer_.Serialize(
      std::move(callback), ftl::MakeCopyable([
        this, promise = std::move(promise), key = std::move(key), priority
      ](Page::PutWithPriorityCallback callback) mutable {
        promise->Finalize(ftl::MakeCopyable([
          this, key = std::move(key), priority, callback = std::move(callback)
        ](storage::Status status, storage::ObjectId object_id) mutable {
          if (status != storage::Status::OK) {
            callback(PageUtils::ConvertStatus(status));
            return;
          }

          PutInCommit(std::move(key), std::move(object_id),
                      priority == Priority::EAGER ? storage::KeyPriority::EAGER
                                                  : storage::KeyPriority::LAZY,
                      std::move(callback));
        }));
      }));
}

// PutReference(array<uint8> key, Reference? reference, Priority priority)
//   => (Status status);
void PageDelegate::PutReference(fidl::Array<uint8_t> key,
                                ReferencePtr reference,
                                Priority priority,
                                const Page::PutReferenceCallback& callback) {
  auto promise = callback::
      Promise<storage::Status, std::unique_ptr<const storage::Object>>::Create(
          storage::Status::ILLEGAL_STATE);
  storage_->GetObject(reference->opaque_id,
                      storage::PageStorage::Location::LOCAL,
                      promise->NewCallback());

  operation_serializer_.Serialize(
      std::move(callback), ftl::MakeCopyable([
        this, promise = std::move(promise), key = std::move(key),
        object_id = std::move(reference->opaque_id), priority
      ](Page::PutReferenceCallback callback) mutable {
        promise->Finalize(ftl::MakeCopyable([
          this, key = std::move(key), object_id = std::move(object_id),
          priority, callback = std::move(callback)
        ](storage::Status status,
          std::unique_ptr<const storage::Object> object) mutable {
          if (status != storage::Status::OK) {
            callback(
                PageUtils::ConvertStatus(status, Status::REFERENCE_NOT_FOUND));
            return;
          }
          PutInCommit(std::move(key), convert::ToString(object_id),
                      priority == Priority::EAGER ? storage::KeyPriority::EAGER
                                                  : storage::KeyPriority::LAZY,
                      std::move(callback));
        }));
      }));
}

// Delete(array<uint8> key) => (Status status);
void PageDelegate::Delete(fidl::Array<uint8_t> key,
                          const Page::DeleteCallback& callback) {
  operation_serializer_.Serialize(
      std::move(callback), ftl::MakeCopyable([ this, key = std::move(key) ](
                               Page::DeleteCallback callback) mutable {

        RunInTransaction(
            ftl::MakeCopyable([key = std::move(key)](storage::Journal *
                                                     journal) mutable {
              return PageUtils::ConvertStatus(journal->Delete(std::move(key)),
                                              Status::KEY_NOT_FOUND);
            }),
            std::move(callback));
      }));
}

// CreateReference(uint64 size, handle<socket> data)
//   => (Status status, Reference reference);
void PageDelegate::CreateReference(
    uint64_t size,
    mx::socket data,
    const Page::CreateReferenceCallback& callback) {
  storage_->AddObjectFromLocal(
      storage::DataSource::Create(std::move(data), size),
      [callback = std::move(callback)](storage::Status status,
                                       storage::ObjectId object_id) {
        if (status != storage::Status::OK) {
          callback(PageUtils::ConvertStatus(status), nullptr);
          return;
        }

        ReferencePtr reference = Reference::New();
        reference->opaque_id = convert::ToArray(object_id);
        callback(Status::OK, std::move(reference));
      });
}

// StartTransaction() => (Status status);
void PageDelegate::StartTransaction(
    const Page::StartTransactionCallback& callback) {
  operation_serializer_.Serialize(
      std::move(callback), [this](StatusCallback callback) {
        if (journal_) {
          callback(Status::TRANSACTION_ALREADY_IN_PROGRESS);
          return;
        }
        storage::CommitId commit_id = branch_tracker_.GetBranchHeadId();
        storage::Status status = storage_->StartCommit(
            commit_id, storage::JournalType::EXPLICIT, &journal_);
        if (status != storage::Status::OK) {
          callback(PageUtils::ConvertStatus(status));
          return;
        }
        journal_parent_commit_ = commit_id;
        branch_tracker_.StartTransaction([callback = std::move(callback)]() {
          callback(Status::OK);
        });
      });
}

// Commit() => (Status status);
void PageDelegate::Commit(const Page::CommitCallback& callback) {
  operation_serializer_.Serialize(
      std::move(callback), [this](StatusCallback callback) {
        if (!journal_) {
          callback(Status::NO_TRANSACTION_IN_PROGRESS);
          return;
        }
        journal_parent_commit_.clear();
        CommitJournal(std::move(journal_), [
          this, callback = std::move(callback)
        ](Status status, std::unique_ptr<const storage::Commit> commit) {
          branch_tracker_.StopTransaction(std::move(commit));
          callback(status);
        });
      });
}

// Rollback() => (Status status);
void PageDelegate::Rollback(const Page::RollbackCallback& callback) {
  operation_serializer_.Serialize(
      std::move(callback), [this](StatusCallback callback) {
        if (!journal_) {
          callback(Status::NO_TRANSACTION_IN_PROGRESS);
          return;
        }
        storage::Status status = journal_->Rollback();
        journal_.reset();
        journal_parent_commit_.clear();
        callback(PageUtils::ConvertStatus(status));
        branch_tracker_.StopTransaction(nullptr);
      });
}

const storage::CommitId& PageDelegate::GetCurrentCommitId() {
  // TODO(etiennej): Commit implicit transactions when we have those.
  if (!journal_) {
    return branch_tracker_.GetBranchHeadId();
  } else {
    return journal_parent_commit_;
  }
}

void PageDelegate::PutInCommit(fidl::Array<uint8_t> key,
                               storage::ObjectId object_id,
                               storage::KeyPriority priority,
                               std::function<void(Status)> callback) {
  RunInTransaction(
      ftl::MakeCopyable([
        key = std::move(key), object_id = std::move(object_id), priority
      ](storage::Journal * journal) mutable {
        return PageUtils::ConvertStatus(
            journal->Put(std::move(key), std::move(object_id), priority));
      }),
      std::move(callback));
}

void PageDelegate::RunInTransaction(
    std::function<Status(storage::Journal* journal)> runnable,
    std::function<void(Status)> callback) {
  if (journal_) {
    // A transaction is in progress; add this change to it.
    callback(runnable(journal_.get()));
    return;
  }
  // No transaction is in progress; create one just for this change.
  // TODO(etiennej): Add a change batching strategy for operations outside
  // transactions. Currently, we create a commit for every change; we
  // would like to group changes that happen "close enough" together in
  // one commit.
  branch_tracker_.StartTransaction([] {});
  storage::CommitId commit_id = branch_tracker_.GetBranchHeadId();
  std::unique_ptr<storage::Journal> journal;
  storage::Status status = storage_->StartCommit(
      commit_id, storage::JournalType::IMPLICIT, &journal);
  if (status != storage::Status::OK) {
    callback(PageUtils::ConvertStatus(status));
    journal->Rollback();
    branch_tracker_.StopTransaction(nullptr);
    return;
  }
  Status ledger_status = runnable(journal.get());
  if (ledger_status != Status::OK) {
    callback(ledger_status);
    journal->Rollback();
    branch_tracker_.StopTransaction(nullptr);
    return;
  }

  CommitJournal(std::move(journal), [
    this, callback = std::move(callback)
  ](Status status, std::unique_ptr<const storage::Commit> commit) {
    branch_tracker_.StopTransaction(status == Status::OK ? std::move(commit)
                                                         : nullptr);
    callback(status);
  });
}

void PageDelegate::CommitJournal(
    std::unique_ptr<storage::Journal> journal,
    std::function<void(Status, std::unique_ptr<const storage::Commit>)>
        callback) {
  storage::Journal* journal_ptr = journal.get();
  in_progress_journals_.push_back(std::move(journal));

  journal_ptr->Commit([this, callback, journal_ptr](
                          storage::Status status,
                          std::unique_ptr<const storage::Commit> commit) {
    in_progress_journals_.erase(std::remove_if(
        in_progress_journals_.begin(), in_progress_journals_.end(),
        [&journal_ptr](const std::unique_ptr<storage::Journal>& journal) {
          return journal_ptr == journal.get();
        }));
    callback(PageUtils::ConvertStatus(status), std::move(commit));
  });
}

void PageDelegate::CheckEmpty() {
  if (on_empty_callback_ && !interface_.is_bound() &&
      branch_tracker_.IsEmpty() && operation_serializer_.empty()) {
    on_empty_callback_();
  }
}

}  // namespace ledger
