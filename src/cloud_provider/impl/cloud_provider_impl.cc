// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/cloud_provider/impl/cloud_provider_impl.h"

#include "apps/ledger/src/cloud_provider/impl/encoding.h"
#include "apps/ledger/src/cloud_provider/impl/timestamp_conversions.h"
#include "apps/ledger/src/firebase/encoding.h"
#include "apps/ledger/src/firebase/status.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/strings/concatenate.h"
#include "lib/ftl/strings/string_number_conversions.h"
#include "lib/ftl/strings/string_view.h"
#include "lib/mtl/socket/strings.h"
#include "lib/mtl/vmo/strings.h"

namespace cloud_provider {
namespace {
// The root path under which all commits are stored.
constexpr ftl::StringView kCommitRoot = "commits";
}  // namespace

CloudProviderImpl::CloudProviderImpl(firebase::Firebase* firebase,
                                     gcs::CloudStorage* cloud_storage)
    : firebase_(firebase), cloud_storage_(cloud_storage) {}

CloudProviderImpl::~CloudProviderImpl() {}

void CloudProviderImpl::AddCommits(
    const std::string& auth_token,
    std::vector<Commit> commits,
    const std::function<void(Status)>& callback) {
  std::string encoded_batch;
  bool ok = EncodeCommits(commits, &encoded_batch);
  FTL_DCHECK(ok);

  firebase_->Patch(kCommitRoot.ToString(), GetQueryParams(auth_token, ""),
                   encoded_batch, [callback](firebase::Status status) {
                     callback(ConvertFirebaseStatus(status));
                   });
}

void CloudProviderImpl::WatchCommits(const std::string& auth_token,
                                     const std::string& min_timestamp,
                                     CommitWatcher* watcher) {
  watchers_[watcher] = std::make_unique<WatchClientImpl>(
      firebase_, kCommitRoot.ToString(),
      GetQueryParams(auth_token, min_timestamp), watcher);
}

void CloudProviderImpl::UnwatchCommits(CommitWatcher* watcher) {
  watchers_.erase(watcher);
}

void CloudProviderImpl::GetCommits(
    const std::string& auth_token,
    const std::string& min_timestamp,
    std::function<void(Status, std::vector<Record>)> callback) {
  firebase_->Get(
      kCommitRoot.ToString(), GetQueryParams(auth_token, min_timestamp),
      [callback](firebase::Status status, const rapidjson::Value& value) {
        if (status != firebase::Status::OK) {
          callback(ConvertFirebaseStatus(status), std::vector<Record>());
          return;
        }
        if (value.IsNull()) {
          // No commits synced for this page yet.
          callback(Status::OK, std::vector<Record>());
          return;
        }
        if (!value.IsObject()) {
          callback(Status::PARSE_ERROR, std::vector<Record>());
          return;
        }
        std::vector<Record> records;
        if (!DecodeMultipleCommitsFromValue(value, &records)) {
          callback(Status::PARSE_ERROR, std::vector<Record>());
          return;
        }
        callback(Status::OK, std::move(records));
      });
}

void CloudProviderImpl::AddObject(const std::string& auth_token,
                                  ObjectIdView object_id,
                                  mx::vmo data,
                                  std::function<void(Status)> callback) {
  // Even though this yields path to be used in GCS, we use Firebase key
  // encoding, as it happens to produce valid GCS object names. To be revisited
  // when we redo the encoding in LE-118.
  cloud_storage_->UploadObject(
      auth_token, firebase::EncodeKey(object_id),
      std::move(data), [callback = std::move(callback)](gcs::Status status) {
        callback(ConvertGcsStatus(status));
      });
}

void CloudProviderImpl::GetObject(
    const std::string& auth_token,
    ObjectIdView object_id,
    std::function<void(Status status, uint64_t size, mx::socket data)>
        callback) {
  cloud_storage_->DownloadObject(
      auth_token, firebase::EncodeKey(object_id),
      [callback = std::move(callback)](gcs::Status status, uint64_t size,
                                       mx::socket data) {
        callback(ConvertGcsStatus(status), size, std::move(data));
      });
}

std::vector<std::string> CloudProviderImpl::GetQueryParams(
    const std::string& auth_token,
    const std::string& min_timestamp) {
  std::vector<std::string> result;

  if (!auth_token.empty()) {
    result.push_back("auth=" + auth_token);
  }

  if (!min_timestamp.empty()) {
    result.emplace_back("orderBy=\"timestamp\"");
    result.push_back("startAt=" + ftl::NumberToString(
                                      BytesToServerTimestamp(min_timestamp)));
  }

  return result;
}

}  // namespace cloud_provider
