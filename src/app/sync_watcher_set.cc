// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/sync_watcher_set.h"

namespace ledger {
namespace {
SyncState ConvertToSyncState(cloud_sync::DownloadSyncState download) {
  switch (download) {
    case cloud_sync::DOWNLOAD_IDLE:
      return SyncState::IDLE;
    case cloud_sync::CATCH_UP_DOWNLOAD:
      return SyncState::IN_PROGRESS;
    case cloud_sync::REMOTE_COMMIT_DOWNLOAD:
      return SyncState::IN_PROGRESS;
    case cloud_sync::DOWNLOAD_ERROR:
      return SyncState::ERROR;
  }
}

SyncState ConvertToSyncState(cloud_sync::UploadSyncState upload) {
  switch (upload) {
    case cloud_sync::UPLOAD_IDLE:
      return SyncState::IDLE;
    case cloud_sync::UPLOAD_PENDING:
      return SyncState::PENDING;
    case cloud_sync::WAIT_TOO_MANY_LOCAL_HEADS:
      return SyncState::PENDING;
    case cloud_sync::WAIT_CATCH_UP_DOWNLOAD:
      return SyncState::PENDING;
    case cloud_sync::WAIT_REMOTE_DOWNLOAD:
      return SyncState::PENDING;
    case cloud_sync::UPLOAD_IN_PROGRESS:
      return SyncState::IN_PROGRESS;
    case cloud_sync::UPLOAD_ERROR:
      return SyncState::ERROR;
  }
}

}  // namespace

class SyncWatcherSet::SyncWatcherContainer
    : public cloud_sync::SyncStateWatcher {
 public:
  SyncWatcherContainer(SyncWatcherPtr watcher) : watcher_(std::move(watcher)) {}

  ~SyncWatcherContainer() {}

  void Start(SyncStateContainer base_state) {
    pending_ = base_state;
    Send();
  }

  void Notify(SyncStateContainer sync_state) override {
    if (sync_state == pending_) {
      return;
    }
    pending_ = sync_state;

    SendIfPending();
  }

  void set_on_empty(ftl::Closure on_empty_callback) {
    if (on_empty_callback) {
      watcher_.set_connection_error_handler(std::move(on_empty_callback));
    }
  }

 private:
  void SendIfPending() {
    if (!watcher_ || notification_in_progress_ || last_ == pending_) {
      return;
    }
    Send();
  }

  void Send() {
    notification_in_progress_ = true;
    last_ = pending_;
    watcher_->SyncStateChanged(ConvertToSyncState(last_.download),
                               ConvertToSyncState(last_.upload), [this]() {
                                 notification_in_progress_ = false;
                                 SendIfPending();
                               });
  }

  // fidl interface to the client.
  ledger::SyncWatcherPtr watcher_;

  // True if a notification has been sent but not acknowledged by the client.
  bool notification_in_progress_ = false;
  // pending_ contains the next synchronization state to send to the watcher,
  // or the current one if no notification is currently in progress
  SyncStateContainer pending_;
  // last_ contains the last sent notification.
  SyncStateContainer last_;

  FTL_DISALLOW_COPY_AND_ASSIGN(SyncWatcherContainer);
};

SyncWatcherSet::SyncWatcherSet() {}

SyncWatcherSet::~SyncWatcherSet() {}

void SyncWatcherSet::AddSyncWatcher(
    fidl::InterfaceHandle<SyncWatcher> watcher) {
  SyncWatcherContainer& container =
      watchers_.emplace(SyncWatcherPtr::Create(std::move(watcher)));
  container.Start(current_);
}

void SyncWatcherSet::Notify(SyncStateContainer sync_state) {
  if (current_ == sync_state) {
    // Skip notifying if nothing has changed.
    return;
  }
  current_ = sync_state;
  for (SyncWatcherContainer& watcher : watchers_) {
    watcher.Notify(current_);
  }
}

}  // namespace ledger
