// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/cloud_sync/impl/page_sync_impl.h"

#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "apps/ledger/src/cloud_sync/impl/constants.h"
#include "apps/ledger/src/storage/public/types.h"
#include "lib/ftl/logging.h"

namespace cloud_sync {

PageSyncImpl::PageSyncImpl(ftl::RefPtr<ftl::TaskRunner> task_runner,
                           storage::PageStorage* storage,
                           cloud_provider::CloudProvider* cloud_provider,
                           std::unique_ptr<backoff::Backoff> backoff,
                           ftl::Closure on_error)
    : task_runner_(task_runner),
      storage_(storage),
      cloud_provider_(cloud_provider),
      backoff_(std::move(backoff)),
      on_error_(on_error),
      log_prefix_("Page " + convert::ToHex(storage->GetId()) + " sync: "),
      weak_factory_(this) {
  FTL_DCHECK(storage);
  FTL_DCHECK(cloud_provider);
}

PageSyncImpl::~PageSyncImpl() {
  // Remove the watchers and the delegate, if they were not already removed on
  // hard error.
  if (!errored_) {
    storage_->SetSyncDelegate(nullptr);
    storage_->RemoveCommitWatcher(this);
    cloud_provider_->UnwatchCommits(this);
  }
}

void PageSyncImpl::Start() {
  FTL_DCHECK(!started_);
  started_ = true;
  storage_->SetSyncDelegate(this);

  StartDownload();
}

void PageSyncImpl::SetOnIdle(ftl::Closure on_idle) {
  FTL_DCHECK(!on_idle_);
  FTL_DCHECK(!started_);
  on_idle_ = std::move(on_idle);
}

bool PageSyncImpl::IsIdle() {
  return commit_uploads_.empty() && download_list_retrieved_ &&
         !batch_download_ && commits_to_download_.empty();
}

void PageSyncImpl::SetOnBacklogDownloaded(ftl::Closure on_backlog_downloaded) {
  FTL_DCHECK(!on_backlog_downloaded_);
  FTL_DCHECK(!started_);
  on_backlog_downloaded_ = on_backlog_downloaded;
}

void PageSyncImpl::OnNewCommits(
    const std::vector<std::unique_ptr<const storage::Commit>>& commits,
    storage::ChangeSource source) {
  // Only upload the locally created commits.
  // TODO(ppi): revisit this when we have p2p sync, too.
  if (source != storage::ChangeSource::LOCAL) {
    return;
  }

  std::vector<std::unique_ptr<const storage::Commit>> cloned_commits;
  cloned_commits.reserve(commits.size());
  for (const auto& commit : commits) {
    cloned_commits.push_back(commit->Clone());
  }

  HandleLocalCommits(std::move(cloned_commits));
}

void PageSyncImpl::GetObject(
    storage::ObjectIdView object_id,
    std::function<void(storage::Status status, uint64_t size, mx::socket data)>
        callback) {
  cloud_provider_->GetObject(object_id, [
    this, object_id = object_id.ToString(), callback
  ](cloud_provider::Status status, uint64_t size, mx::socket data) {
    if (status == cloud_provider::Status::NETWORK_ERROR) {
      FTL_LOG(WARNING)
          << "GetObject() failed due to a connection error, retrying.";
      Retry([
        this, object_id = std::move(object_id), callback = std::move(callback)
      ] { GetObject(object_id, callback); });
      return;
    }

    backoff_->Reset();
    if (status != cloud_provider::Status::OK) {
      FTL_LOG(WARNING) << "Fetching remote object failed with status: "
                       << status;
      callback(storage::Status::IO_ERROR, 0, mx::socket());
      return;
    }

    callback(storage::Status::OK, size, std::move(data));
  });
}

void PageSyncImpl::OnRemoteCommit(cloud_provider::Commit commit,
                                  std::string timestamp) {
  std::vector<cloud_provider::Record> records;
  records.emplace_back(std::move(commit), std::move(timestamp));
  if (batch_download_) {
    // If there is already a commit batch being downloaded, save the new commits
    // to be downloaded when it is done.
    std::move(records.begin(), records.end(),
              std::back_inserter(commits_to_download_));
    return;
  }

  DownloadBatch(std::move(records), nullptr);
}

void PageSyncImpl::OnConnectionError() {
  FTL_DCHECK(remote_watch_set_);
  // Reset the watcher and schedule a retry.
  cloud_provider_->UnwatchCommits(this);
  remote_watch_set_ = false;
  FTL_LOG(WARNING)
      << "Connection error in the remote commit watcher, retrying.";
  Retry([this] { SetRemoteWatcher(); });
}

void PageSyncImpl::OnMalformedNotification() {
  HandleError("Received a malformed remote commit notification.");
}

void PageSyncImpl::StartDownload() {
  // Retrieve the server-side timestamp of the last commit we received.
  std::string last_commit_ts;
  auto status = storage_->GetSyncMetadata(kTimestampKey, &last_commit_ts);
  // NOT_FOUND means that we haven't persisted the state yet, e.g. because we
  // haven't received any remote commits yet. In this case an empty timestamp is
  // the right value.
  if (status != storage::Status::OK && status != storage::Status::NOT_FOUND) {
    HandleError("Failed to retrieve the sync metadata.");
    return;
  }
  if (last_commit_ts.empty()) {
    FTL_VLOG(1) << log_prefix_ << "starting sync for the first time, "
                << "retrieving all remote commits";
  } else {
    // TODO(ppi): print the timestamp out as human-readable wall time.
    FTL_VLOG(1) << log_prefix_ << "starting sync again, "
                << "retrieving commits uploaded after: " << last_commit_ts;
  }

  // TODO(ppi): handle pagination when the response is huge.
  cloud_provider_->GetCommits(
      std::move(last_commit_ts),
      [this](cloud_provider::Status cloud_status,
             std::vector<cloud_provider::Record> records) {
        if (cloud_status != cloud_provider::Status::OK) {
          // Fetching the remote commits failed, schedule a retry.
          FTL_LOG(WARNING) << log_prefix_
                           << "fetching the remote commits failed due to a "
                           << "connection error, status: " << cloud_status
                           << ", retrying.";
          Retry([this] { StartDownload(); });
          return;
        }
        backoff_->Reset();

        if (records.empty()) {
          // If there is no remote commits to add, announce that we're done.
          FTL_VLOG(1) << log_prefix_
                      << "initial sync finished, no new remote commits";
          BacklogDownloaded();
        } else {
          FTL_VLOG(1) << log_prefix_ << "retrieved " << records.size()
                      << " (possibly) new remote commits, "
                      << "adding them to storage.";
          // If not, fire the backlog download callback when the remote commits
          // are downloaded.
          const auto record_count = records.size();
          DownloadBatch(std::move(records), [this, record_count] {
            FTL_VLOG(1) << log_prefix_ << "initial sync finished, added "
                        << record_count << " remote commits.";
            BacklogDownloaded();
          });
        }
      });
}

void PageSyncImpl::StartUpload() {
  // Retrieve the backlog of the existing unsynced commits and enqueue them for
  // upload.
  // TODO(ppi): either switch to a paginating API or (better?) ensure that long
  // backlogs of local commits are squashed in storage, as otherwise the list of
  // commits can be possibly very big.
  storage_->GetUnsyncedCommits(
      [this](storage::Status status,
             std::vector<std::unique_ptr<const storage::Commit>> commits) {
        if (status != storage::Status::OK) {
          HandleError("Failed to retrieve the unsynced commits");
          return;
        }

        HandleLocalCommits(std::move(commits));

        // Subscribe to notifications about new commits in Storage.
        storage_->AddCommitWatcher(this);
        local_watch_set_ = true;
      });
}

void PageSyncImpl::DownloadBatch(std::vector<cloud_provider::Record> records,
                                 ftl::Closure on_done) {
  FTL_DCHECK(!batch_download_);
  batch_download_ = std::make_unique<BatchDownload>(
      storage_, std::move(records), [ this, on_done = std::move(on_done) ] {
        if (on_done) {
          on_done();
        }
        batch_download_.reset();

        if (commits_to_download_.empty()) {
          if (!commits_staged_for_upload_.empty()) {
            HandleLocalCommits(
                std::vector<std::unique_ptr<const storage::Commit>>());
          }
          CheckIdle();
          return;
        }
        auto commits = std::move(commits_to_download_);
        commits_to_download_.clear();
        DownloadBatch(std::move(commits), nullptr);
      },
      [this] { HandleError("Failed to persist a remote commit in storage"); });
  batch_download_->Start();
}

void PageSyncImpl::SetRemoteWatcher() {
  FTL_DCHECK(!remote_watch_set_);
  // Retrieve the server-side timestamp of the last commit we received.
  std::string last_commit_ts;
  auto status = storage_->GetSyncMetadata(kTimestampKey, &last_commit_ts);
  if (status != storage::Status::OK && status != storage::Status::NOT_FOUND) {
    HandleError("Failed to retrieve the sync metadata.");
    return;
  }

  cloud_provider_->WatchCommits(last_commit_ts, this);
  remote_watch_set_ = true;
}

void PageSyncImpl::HandleLocalCommits(
    std::vector<std::unique_ptr<const storage::Commit>> commits) {
  if (batch_download_) {
    // If a commit is currently downloaded, stage the upload until it is done.
    std::move(std::begin(commits), std::end(commits),
              std::back_inserter(commits_staged_for_upload_));
    return;
  }

  std::vector<storage::CommitId> heads;
  if (storage_->GetHeadCommitIds(&heads) != storage::Status::OK) {
    HandleError("Failed to retrieve the current heads");
    return;
  }
  FTL_DCHECK(!heads.empty());

  if (heads.size() > 1u) {
    // To many local heads, stage commits for upload but don't enqueue yet.
    std::move(std::begin(commits), std::end(commits),
              std::back_inserter(commits_staged_for_upload_));
    return;
  }

  // Only one local head - enqueue the commits previously staged for upload,
  // then the new commits.
  for (auto& staged_commit : commits_staged_for_upload_) {
    EnqueueUpload(std::move(staged_commit));
  }
  commits_staged_for_upload_.clear();
  for (auto& commit : commits) {
    EnqueueUpload(std::move(commit));
  }
}

void PageSyncImpl::EnqueueUpload(
    std::unique_ptr<const storage::Commit> commit) {
  // If there are no commits currently being uploaded, start the upload after
  // enqueing this one.
  const bool start_after_adding = commit_uploads_.empty();

  commit_uploads_.emplace(
      storage_, cloud_provider_, std::move(commit),
      [this] {
        // Upload succeeded, reset the backoff delay.
        backoff_->Reset();

        commit_uploads_.pop();
        if (!commit_uploads_.empty()) {
          commit_uploads_.front().Start();
        } else {
          CheckIdle();
        }
      },
      [this] {
        FTL_LOG(WARNING)
            << log_prefix_
            << "commit upload failed due to a connection error, retrying.";
        Retry([this] { commit_uploads_.front().Start(); });
      });

  if (start_after_adding) {
    commit_uploads_.front().Start();
  }
}

void PageSyncImpl::Retry(ftl::Closure callable) {
  task_runner_->PostDelayedTask(
      [
        weak_this = weak_factory_.GetWeakPtr(), callable = std::move(callable)
      ]() {
        if (weak_this && !weak_this->errored_) {
          callable();
        }
      },
      backoff_->GetNext());
}

void PageSyncImpl::HandleError(const char error_description[]) {
  FTL_LOG(ERROR) << log_prefix_ << error_description << " Stopping sync.";
  if (local_watch_set_) {
    storage_->RemoveCommitWatcher(this);
  }
  if (remote_watch_set_) {
    cloud_provider_->UnwatchCommits(this);
  }
  storage_->SetSyncDelegate(nullptr);
  on_error_();
  errored_ = true;
}

void PageSyncImpl::CheckIdle() {
  if (on_idle_ && IsIdle()) {
    on_idle_();
  }
}

void PageSyncImpl::BacklogDownloaded() {
  download_list_retrieved_ = true;
  if (on_backlog_downloaded_) {
    on_backlog_downloaded_();
  }
  SetRemoteWatcher();
  StartUpload();
  CheckIdle();
}

}  // namespace cloud_sync
