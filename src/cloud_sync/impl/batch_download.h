// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_CLOUD_SYNC_IMPL_BATCH_DOWNLOAD_H_
#define APPS_LEDGER_SRC_CLOUD_SYNC_IMPL_BATCH_DOWNLOAD_H_

#include "apps/ledger/src/cloud_provider/public/record.h"
#include "apps/ledger/src/storage/public/page_storage.h"
#include "lib/ftl/functional/closure.h"

namespace cloud_sync {

// Adds a batch of remote commits to storage.
//
// Given a list of commit metadata, this class makes a request to add them to
// storage, and waits until storage confirms that the operation completed before
// calling |on_done|.
//
// The operation is not retryable, and errors reported through |on_error| are
// not recoverable.
class BatchDownload {
 public:
  BatchDownload(storage::PageStorage* storage,
                std::vector<cloud_provider::Record> records,
                ftl::Closure on_done,
                ftl::Closure on_error);
  ~BatchDownload();

  // Can be called only once.
  void Start();

 private:
  void UpdateTimestampAndQuit();

  storage::PageStorage* const storage_;
  std::vector<cloud_provider::Record> records_;
  ftl::Closure on_done_;
  ftl::Closure on_error_;
  bool started_ = false;

  FTL_DISALLOW_COPY_AND_ASSIGN(BatchDownload);
};

}  // namespace cloud_sync

#endif  // APPS_LEDGER_SRC_CLOUD_SYNC_IMPL_BATCH_DOWNLOAD_H_
