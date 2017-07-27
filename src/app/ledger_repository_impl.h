// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_APP_LEDGER_REPOSITORY_IMPL_H_
#define APPS_LEDGER_SRC_APP_LEDGER_REPOSITORY_IMPL_H_

#include "apps/ledger/services/internal/internal.fidl.h"
#include "apps/ledger/services/public/ledger.fidl.h"
#include "apps/ledger/src/app/ledger_manager.h"
#include "apps/ledger/src/app/sync_watcher_set.h"
#include "apps/ledger/src/callback/auto_cleanable.h"
#include "apps/ledger/src/cloud_sync/public/user_config.h"
#include "apps/ledger/src/cloud_sync/public/user_sync.h"
#include "apps/ledger/src/convert/convert.h"
#include "apps/ledger/src/environment/environment.h"
#include "lib/fidl/cpp/bindings/binding_set.h"
#include "lib/fidl/cpp/bindings/interface_ptr_set.h"
#include "lib/ftl/macros.h"

namespace ledger {

class LedgerRepositoryImpl : public LedgerRepository {
 public:
  LedgerRepositoryImpl(std::string base_storage_dir,
                       Environment* environment,
                       std::unique_ptr<SyncWatcherSet> watchers,
                       std::unique_ptr<cloud_sync::UserSync> user_sync);
  ~LedgerRepositoryImpl() override;

  void set_on_empty(const ftl::Closure& on_empty_callback) {
    on_empty_callback_ = on_empty_callback;
  }

  void BindRepository(
      fidl::InterfaceRequest<LedgerRepository> repository_request);

  // Releases all handles bound to this repository impl.
  std::vector<fidl::InterfaceRequest<LedgerRepository>> Unbind();

 private:
  // LedgerRepository:
  void GetLedger(fidl::Array<uint8_t> ledger_name,
                 fidl::InterfaceRequest<Ledger> ledger_request,
                 const GetLedgerCallback& callback) override;
  void Duplicate(fidl::InterfaceRequest<LedgerRepository> request,
                 const DuplicateCallback& callback) override;
  void SetSyncStateWatcher(
      fidl::InterfaceHandle<SyncWatcher> watcher,
      const SetSyncStateWatcherCallback& callback) override;

  void CheckEmpty();

  const std::string base_storage_dir_;
  Environment* const environment_;
  std::unique_ptr<SyncWatcherSet> watchers_;
  std::unique_ptr<cloud_sync::UserSync> user_sync_;
  callback::AutoCleanableMap<std::string,
                             LedgerManager,
                             convert::StringViewComparator>
      ledger_managers_;
  fidl::BindingSet<LedgerRepository> bindings_;
  ftl::Closure on_empty_callback_;

  FTL_DISALLOW_COPY_AND_ASSIGN(LedgerRepositoryImpl);
};

}  // namespace ledger

#endif  // APPS_LEDGER_SRC_APP_LEDGER_REPOSITORY_IMPL_H_
