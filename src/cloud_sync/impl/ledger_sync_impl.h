// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_CLOUD_SYNC_IMPL_LEDGER_SYNC_IMPL_H_
#define APPS_LEDGER_SRC_CLOUD_SYNC_IMPL_LEDGER_SYNC_IMPL_H_

#include <memory>
#include <unordered_set>

#include "apps/ledger/src/cloud_sync/impl/aggregator.h"
#include "apps/ledger/src/cloud_sync/impl/page_sync_impl.h"
#include "apps/ledger/src/cloud_sync/public/ledger_sync.h"
#include "apps/ledger/src/cloud_sync/public/sync_state_watcher.h"
#include "apps/ledger/src/cloud_sync/public/user_config.h"
#include "apps/ledger/src/environment/environment.h"
#include "apps/ledger/src/firebase/firebase.h"
#include "apps/ledger/src/network/network_service.h"

namespace cloud_sync {

class LedgerSyncImpl : public LedgerSync {
 public:
  LedgerSyncImpl(ledger::Environment* environment,
                 const UserConfig* user_config,
                 ftl::StringView app_id,
                 std::unique_ptr<SyncStateWatcher> watcher);
  ~LedgerSyncImpl();

  std::unique_ptr<PageSyncContext> CreatePageContext(
      storage::PageStorage* page_storage,
      ftl::Closure error_callback) override;

  // Enables upload. Has no effect if this method has already been called.
  void EnableUpload();

  // |on_delete| will be called when this class is deleted.
  void set_on_delete(std::function<void()> on_delete) {
    FTL_DCHECK(!on_delete_);
    on_delete_ = on_delete;
  }

 private:
  ledger::Environment* const environment_;
  const UserConfig* const user_config_;
  bool upload_enabled_ = false;
  const std::string app_gcs_prefix_;
  // Firebase path under which the data of this Ledger instance is stored.
  const std::string app_firebase_path_;
  // Firebase instance scoped to |app_path_|.
  std::unique_ptr<firebase::Firebase> app_firebase_;
  std::unordered_set<PageSyncImpl*> active_page_syncs_;
  // Called on destruction.
  std::function<void()> on_delete_;
  std::unique_ptr<SyncStateWatcher> user_watcher_;
  Aggregator aggregator_;
};

}  // namespace cloud_sync

#endif  // APPS_LEDGER_SRC_CLOUD_SYNC_IMPL_LEDGER_SYNC_IMPL_H_
