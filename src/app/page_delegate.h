// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_APP_PAGE_DELEGATE_H_
#define APPS_LEDGER_SRC_APP_PAGE_DELEGATE_H_

#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "apps/ledger/services/public/ledger.fidl.h"
#include "apps/ledger/src/app/branch_tracker.h"
#include "apps/ledger/src/app/fidl/bound_interface.h"
#include "apps/ledger/src/app/page_impl.h"
#include "apps/ledger/src/callback/operation_serializer.h"
#include "apps/ledger/src/storage/public/data_source.h"
#include "apps/ledger/src/storage/public/journal.h"
#include "apps/ledger/src/storage/public/page_storage.h"
#include "apps/ledger/src/storage/public/types.h"
#include "lib/ftl/macros.h"

namespace ledger {
class PageManager;

// A delegate for the implementation of the |Page| interface.
//
// PageDelegate owns PageImpl and BranchTracker. It makes sure that all
// operations in progress will terminate, even if the Page is no longer
// connected. When the page connection is closed and BranchTracker is also
// empty, the client is notified through |on_empty_callback| (registered by
// |set_on_empty()|).
class PageDelegate {
 public:
  PageDelegate(coroutine::CoroutineService* coroutine_service,
               PageManager* manager,
               storage::PageStorage* storage,
               fidl::InterfaceRequest<Page> request);
  ~PageDelegate();

  void set_on_empty(ftl::Closure on_empty_callback) {
    on_empty_callback_ = on_empty_callback;
  }

  // From Page interface, called by PageImpl:
  void GetId(const Page::GetIdCallback& callback);

  void GetSnapshot(fidl::InterfaceRequest<PageSnapshot> snapshot_request,
                   fidl::Array<uint8_t> key_prefix,
                   fidl::InterfaceHandle<PageWatcher> watcher,
                   const Page::GetSnapshotCallback& callback);

  void Put(fidl::Array<uint8_t> key,
           fidl::Array<uint8_t> value,
           const Page::PutCallback& callback);

  void PutWithPriority(fidl::Array<uint8_t> key,
                       fidl::Array<uint8_t> value,
                       Priority priority,
                       const Page::PutWithPriorityCallback& callback);

  void PutReference(fidl::Array<uint8_t> key,
                    ReferencePtr reference,
                    Priority priority,
                    const Page::PutReferenceCallback& callback);

  void Delete(fidl::Array<uint8_t> key, const Page::DeleteCallback& callback);

  void CreateReference(std::unique_ptr<storage::DataSource> data,
                       std::function<void(Status, ReferencePtr)> callback);

  void StartTransaction(const Page::StartTransactionCallback& callback);

  void Commit(const Page::CommitCallback& callback);

  void Rollback(const Page::RollbackCallback& callback);

 private:
  using StatusCallback = std::function<void(Status)>;

  const storage::CommitId& GetCurrentCommitId();

  void PutInCommit(fidl::Array<uint8_t> key,
                   storage::ObjectId value,
                   storage::KeyPriority priority,
                   StatusCallback callback);

  // Run |runnable| in a transaction, and notifies |callback| of the result. If
  // a transaction is currently in progress, reuses it, otherwise creates a new
  // one and commit it before calling |callback|. This method is not
  // serialized, and should only be called from a callsite that is serialized.
  void RunInTransaction(
      std::function<Status(storage::Journal* journal)> runnable,
      StatusCallback callback);

  void CommitJournal(
      std::unique_ptr<storage::Journal> journal,
      std::function<void(Status, std::unique_ptr<const storage::Commit>)>
          callback);

  // Queue operations such that they are serialized: an operation is run only
  // when all previous operations registered through this method have terminated
  // by calling their callbacks. When |operation| terminates, |callback| is
  // called with the status returned by |operation|.
  void SerializeOperation(StatusCallback callback,
                          std::function<void(StatusCallback)> operation);

  void CheckEmpty();

  PageManager* manager_;
  storage::PageStorage* storage_;

  BoundInterface<Page, PageImpl> interface_;
  BranchTracker branch_tracker_;

  ftl::Closure on_empty_callback_;

  storage::CommitId journal_parent_commit_;
  std::unique_ptr<storage::Journal> journal_;
  callback::OperationSerializer<Status> operation_serializer_;
  std::vector<std::unique_ptr<storage::Journal>> in_progress_journals_;

  FTL_DISALLOW_COPY_AND_ASSIGN(PageDelegate);
};

}  // namespace ledger

#endif  // APPS_LEDGER_SRC_APP_PAGE_DELEGATE_H_
