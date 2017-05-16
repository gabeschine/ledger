// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_CLOUD_PROVIDER_IMPL_WATCH_CLIENT_IMPL_H_
#define APPS_LEDGER_SRC_CLOUD_PROVIDER_IMPL_WATCH_CLIENT_IMPL_H_

#include <vector>

#include "apps/ledger/src/cloud_provider/public/commit_watcher.h"
#include "apps/ledger/src/cloud_provider/public/record.h"
#include "apps/ledger/src/firebase/firebase.h"
#include "apps/ledger/src/firebase/watch_client.h"

#include <rapidjson/document.h>

namespace cloud_provider {

// Relay between Firebase and a CommitWatcher corresponding to
// particular WatchCommits() request.
class WatchClientImpl : public firebase::WatchClient {
 public:
  WatchClientImpl(firebase::Firebase* firebase,
                  const std::string& firebase_key,
                  const std::string& query,
                  CommitWatcher* commit_watcher);
  ~WatchClientImpl() override;

  // firebase::WatchClient:
  void OnPut(const std::string& path, const rapidjson::Value& value) override;
  void OnPatch(const std::string& path, const rapidjson::Value& value) override;
  void OnMalformedEvent() override;
  void OnConnectionError() override;

 private:
  void Handle(const std::string& path, const rapidjson::Value& value);

  void ProcessRecord(Record record);

  void CommitBatch();

  void HandleDecodingError(const std::string& path,
                           const rapidjson::Value& value,
                           const char error_description[]);
  void HandleError();

  firebase::Firebase* const firebase_;
  CommitWatcher* const commit_watcher_;
  bool errored_ = false;
  // Commits of the current pending batch.
  std::vector<Record> batch_;
  // Timestamp of the current pending batch.
  std::string batch_timestamp_;
  // Total size of the current pending batch.
  size_t batch_size_;
};

}  // namespace cloud_provider

#endif  // APPS_LEDGER_SRC_CLOUD_PROVIDER_IMPL_WATCH_CLIENT_IMPL_H_
