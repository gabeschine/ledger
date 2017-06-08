// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/cloud_sync/impl/user_sync_impl.h"

#include "apps/ledger/src/cloud_sync/impl/ledger_sync_impl.h"
#include "apps/ledger/src/cloud_sync/impl/paths.h"
#include "apps/ledger/src/firebase/firebase_impl.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/strings/concatenate.h"

namespace cloud_sync {

UserSyncImpl::UserSyncImpl(ledger::Environment* environment,
                           UserConfig user_config,
                           std::unique_ptr<backoff::Backoff> backoff)
    : environment_(environment),
      user_config_(std::move(user_config)),
      backoff_(std::move(backoff)),
      weak_ptr_factory_(this) {}

UserSyncImpl::~UserSyncImpl() {
  FTL_DCHECK(active_ledger_syncs_.empty());
}

std::string UserSyncImpl::GetLocalVersionPath() {
  return ftl::Concatenate({user_config_.user_directory, "/local_version"});
}

void UserSyncImpl::Start() {
  FTL_DCHECK(!started_);

  if (user_config_.use_sync) {
    user_firebase_ = std::make_unique<firebase::FirebaseImpl>(
        environment_->network_service(), user_config_.server_id,
        GetFirebasePathForUser(user_config_.user_id));
    CheckCloudVersion();
  }

  started_ = true;
}

void UserSyncImpl::CheckCloudVersion() {
  FTL_DCHECK(user_firebase_);

  local_version_checker_.CheckCloudVersion(
      user_firebase_.get(), GetLocalVersionPath(),
      [this](LocalVersionChecker::Status status) {
        if (status == LocalVersionChecker::Status::OK) {
          EnableUpload();
          return;
        }

        if (status == LocalVersionChecker::Status::NETWORK_ERROR) {
          // Retry after some backoff time.
          environment_->main_runner()->PostDelayedTask(
              [weak_this = weak_ptr_factory_.GetWeakPtr()] {
                if (weak_this) {
                  weak_this->CheckCloudVersion();
                }
              },
              backoff_->GetNext());
          return;
        }

        if (status == LocalVersionChecker::Status::DISK_ERROR) {
          FTL_LOG(ERROR) << "Unable to access local version file: "
                           << GetLocalVersionPath()
                           << ". Sync upload will be disabled.";
          return;
        }

        FTL_DCHECK(status == LocalVersionChecker::Status::INCOMPATIBLE);
        FTL_LOG(FATAL) << "Version on the cloud is incompatible with local "
                          "version. Very probably the cloud instance has been "
                          "cleaned up and the local state is stale. Clean the "
                          "local state before restarting the Ledger.";
      });
}

void UserSyncImpl::EnableUpload() {
  upload_enabled_ = true;
  for (auto ledger_sync : active_ledger_syncs_) {
    ledger_sync->EnableUpload();
  }
}

std::unique_ptr<LedgerSync> UserSyncImpl::CreateLedgerSync(
    ftl::StringView app_id) {
  FTL_DCHECK(started_);

  if (!user_config_.use_sync) {
    return nullptr;
  }

  auto result =
      std::make_unique<LedgerSyncImpl>(environment_, &user_config_, app_id);
  result->set_on_delete([ this, ledger_sync = result.get() ]() {
    active_ledger_syncs_.erase(ledger_sync);
  });
  active_ledger_syncs_.insert(result.get());
  if (upload_enabled_) {
    result->EnableUpload();
  }
  return result;
}

}  // namespace cloud_sync