// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/cloud_sync/impl/commit_upload.h"

#include "apps/ledger/src/cloud_provider/public/commit.h"
#include "apps/ledger/src/cloud_provider/public/types.h"
#include "lib/ftl/logging.h"
#include "lib/mtl/vmo/strings.h"

namespace cloud_sync {

CommitUpload::CommitUpload(
    storage::PageStorage* storage,
    cloud_provider::CloudProvider* cloud_provider,
    std::vector<std::unique_ptr<const storage::Commit>> commits,
    ftl::Closure on_done,
    ftl::Closure on_error,
    unsigned int max_concurrent_uploads)
    : storage_(storage),
      cloud_provider_(cloud_provider),
      commits_(std::move(commits)),
      on_done_(on_done),
      on_error_(on_error),
      max_concurrent_uploads_(max_concurrent_uploads) {
  FTL_DCHECK(storage);
  FTL_DCHECK(cloud_provider);
}

CommitUpload::~CommitUpload() {}

void CommitUpload::Start() {
  FTL_DCHECK(!started_);
  FTL_DCHECK(!errored_);
  started_ = true;

  storage_->GetAllUnsyncedObjectIds(
      [this](storage::Status status,
             std::vector<storage::ObjectId> object_ids) {
        FTL_DCHECK(status == storage::Status::OK);
        for (auto& object_id : object_ids) {
          remaining_object_ids_.push(std::move(object_id));
        }
        StartObjectUpload();
      });
}

void CommitUpload::Retry() {
  FTL_DCHECK(started_);
  FTL_DCHECK(errored_);
  errored_ = false;
  StartObjectUpload();
}

void CommitUpload::StartObjectUpload() {
  FTL_DCHECK(current_uploads_ == 0u);
  // If there are no unsynced objects left, upload the commits.
  if (remaining_object_ids_.empty()) {
    UploadCommits();
    return;
  }

  while (current_uploads_ < max_concurrent_uploads_ &&
         !remaining_object_ids_.empty()) {
    UploadNextObject();
  }
}

void CommitUpload::UploadNextObject() {
  FTL_DCHECK(!remaining_object_ids_.empty());
  FTL_DCHECK(current_uploads_ < max_concurrent_uploads_);
  current_uploads_++;
  storage_->GetObject(remaining_object_ids_.front(),
                      storage::PageStorage::Location::LOCAL,
                      [this](storage::Status storage_status,
                             std::unique_ptr<const storage::Object> object) {
                        FTL_DCHECK(storage_status == storage::Status::OK);
                        UploadObject(std::move(object));
                      });
  // Pop the object from the queue - if the upload fails, we will re-enqueue it.
  remaining_object_ids_.pop();
}

void CommitUpload::UploadObject(std::unique_ptr<const storage::Object> object) {
  ftl::StringView data_view;
  auto status = object->GetData(&data_view);
  FTL_DCHECK(status == storage::Status::OK);

  // TODO(ppi): get the virtual memory object directly from storage::Object,
  // once it can give us one.
  mx::vmo data;
  auto result = mtl::VmoFromString(data_view, &data);
  FTL_DCHECK(result);

  storage::ObjectId id = object->GetId();
  cloud_provider_->AddObject(
      object->GetId(), std::move(data),
      [ this, id = std::move(id) ](cloud_provider::Status status) {
        FTL_DCHECK(current_uploads_ > 0);
        current_uploads_--;

        if (status != cloud_provider::Status::OK) {
          errored_ = true;
          // Re-enqueue the object for another upload attempt.
          remaining_object_ids_.push(std::move(id));

          if (current_uploads_ == 0u) {
            on_error_();
          }
          return;
        }

        // Uploading the object succeeded.
        auto result = storage_->MarkObjectSynced(id);
        FTL_DCHECK(result == storage::Status::OK);

        // Notify the user about the error once all pending uploads of the
        // recent retry complete.
        if (errored_ && current_uploads_ == 0u) {
          on_error_();
          return;
        }

        if (current_uploads_ == 0 && remaining_object_ids_.empty()) {
          // All the referenced objects are uploaded, upload the commits.
          UploadCommits();
          return;
        }

        if (!errored_ && !remaining_object_ids_.empty()) {
          UploadNextObject();
        }
      });
}

void CommitUpload::UploadCommits() {
  FTL_DCHECK(!errored_);
  std::vector<cloud_provider::Commit> commits;
  std::vector<storage::CommitId> ids;
  for (auto& storage_commit : commits_) {
    storage::CommitId id = storage_commit->GetId();
    cloud_provider::Commit commit(
        id, storage_commit->GetStorageBytes().ToString(),
        std::map<cloud_provider::ObjectId, cloud_provider::Data>{});
    commits.push_back(std::move(commit));
    ids.push_back(std::move(id));
  }
  cloud_provider_->AddCommits(
      std::move(commits),
      [ this, commit_ids = std::move(ids) ](cloud_provider::Status status) {
        // UploadCommit() is called as a last step of a so-far-successful upload
        // attempt, so we couldn't have failed before.
        FTL_DCHECK(!errored_);
        if (status != cloud_provider::Status::OK) {
          errored_ = true;
          on_error_();
          return;
        }
        for (auto& id : commit_ids) {
          auto ret = storage_->MarkCommitSynced(id);
          FTL_DCHECK(ret == storage::Status::OK);
        }
        // This object can be deleted in the on_done_() callback, don't do
        // anything after the call.
        on_done_();
      });
}

}  // namespace cloud_sync
