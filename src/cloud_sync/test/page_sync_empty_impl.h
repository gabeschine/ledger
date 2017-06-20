// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_CLOUD_SYNC_TEST_PAGE_SYNC_EMPTY_IMPL_H_
#define APPS_LEDGER_SRC_CLOUD_SYNC_TEST_PAGE_SYNC_EMPTY_IMPL_H_

#include "apps/ledger/src/cloud_sync/public/page_sync.h"

namespace cloud_sync {
namespace test {

class PageSyncEmptyImpl : public PageSync {
 public:
  // PageSync:
  void Start() override;
  void SetOnIdle(ftl::Closure on_idle_callback) override;
  bool IsIdle() override;
  void SetOnBacklogDownloaded(
      ftl::Closure on_backlog_downloaded_callback) override;
  void SetSyncWatcher(SyncStateWatcher* watcher) override;
};

}  // namespace test
}  // namespace cloud_sync

#endif  // APPS_LEDGER_SRC_CLOUD_SYNC_TEST_PAGE_SYNC_EMPTY_IMPL_H_
