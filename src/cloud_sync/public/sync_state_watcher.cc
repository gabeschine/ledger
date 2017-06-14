// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/cloud_sync/public/sync_state_watcher.h"

namespace cloud_sync {

SyncStateWatcher::SyncStateContainer::SyncStateContainer(
    DownloadSyncState download,
    UploadSyncState upload)
    : download(download), upload(upload) {}

SyncStateWatcher::SyncStateContainer::SyncStateContainer() {}

void SyncStateWatcher::SyncStateContainer::Merge(SyncStateContainer other) {
  if (other.download > this->download) {
    download = other.download;
  }
  if (other.upload > this->upload) {
    upload = other.upload;
  }
}

void SyncStateWatcher::Notify(DownloadSyncState download,
                              UploadSyncState upload) {
  Notify(SyncStateContainer(download, upload));
}

bool operator==(const SyncStateWatcher::SyncStateContainer& lhs,
                const SyncStateWatcher::SyncStateContainer& rhs) {
  return lhs.download == rhs.download && lhs.upload == rhs.upload;
}

bool operator!=(const SyncStateWatcher::SyncStateContainer& lhs,
                const SyncStateWatcher::SyncStateContainer& rhs) {
  return !(lhs == rhs);
}

}  // namespace cloud_sync
