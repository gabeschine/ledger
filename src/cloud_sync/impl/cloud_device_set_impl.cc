// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/cloud_sync/impl/cloud_device_set_impl.h"

#include "lib/ftl/logging.h"
#include "lib/ftl/strings/concatenate.h"

namespace cloud_sync {

namespace {
std::string GetDeviceMapKey(ftl::StringView fingerprint) {
  return ftl::Concatenate({"__metadata/devices/", fingerprint});
}
}  // namespace

CloudDeviceSetImpl::CloudDeviceSetImpl(
    std::unique_ptr<firebase::Firebase> user_firebase)
    : user_firebase_(std::move(user_firebase)) {}

CloudDeviceSetImpl::~CloudDeviceSetImpl() {
  if (firebase_watcher_set_) {
    ResetWatcher();
  }
}

void CloudDeviceSetImpl::CheckFingerprint(
    std::string auth_token,
    std::string fingerprint,
    std::function<void(Status)> callback) {
  std::vector<std::string> query_params;
  if (!auth_token.empty()) {
    query_params.push_back("auth=" + auth_token);
  }

  user_firebase_->Get(
      GetDeviceMapKey(fingerprint), query_params,
      [callback = std::move(callback)](firebase::Status status,
                                       const rapidjson::Value& value) {
        if (status != firebase::Status::OK) {
          FTL_LOG(WARNING) << "Unable to read version from the cloud.";
          callback(Status::NETWORK_ERROR);
          return;
        }

        if (value.IsNull()) {
          callback(Status::ERASED);
          return;
        }

        // If metadata are present, the version on the cloud is compatible.
        callback(Status::OK);
      });
}

void CloudDeviceSetImpl::SetFingerprint(std::string auth_token,
                                        std::string fingerprint,
                                        std::function<void(Status)> callback) {
  std::vector<std::string> query_params;
  if (!auth_token.empty()) {
    query_params.push_back("auth=" + auth_token);
  }

  user_firebase_->Put(
      GetDeviceMapKey(fingerprint), query_params,
      "true", [callback = std::move(callback)](firebase::Status status) {
        if (status != firebase::Status::OK) {
          FTL_LOG(WARNING) << "Unable to set local version on the cloud.";
          callback(Status::NETWORK_ERROR);
          return;
        }

        callback(Status::OK);
      });
}

void CloudDeviceSetImpl::WatchFingerprint(
    std::string auth_token,
    std::string fingerprint,
    std::function<void(Status)> callback) {
  if (firebase_watcher_set_) {
    ResetWatcher();
  }

  std::vector<std::string> query_params;
  if (!auth_token.empty()) {
    query_params.push_back("auth=" + auth_token);
  }

  user_firebase_->Watch(GetDeviceMapKey(fingerprint), query_params, this);
  firebase_watcher_set_ = true;
  watch_callback_ = callback;
}

void CloudDeviceSetImpl::OnPut(const std::string& path,
                               const rapidjson::Value& value) {
  FTL_DCHECK(firebase_watcher_set_ && watch_callback_);
  if (value.IsNull()) {
    if (destruction_sentinel_.DestructedWhile(
            [this] { watch_callback_(Status::ERASED); })) {
      return;
    }
    ResetWatcher();
    return;
  }

  watch_callback_(Status::OK);
}

void CloudDeviceSetImpl::OnPatch(const std::string& path,
                                 const rapidjson::Value& value) {
  FTL_DCHECK(firebase_watcher_set_ && watch_callback_);
  FTL_NOTIMPLEMENTED();
}

void CloudDeviceSetImpl::OnCancel() {
  FTL_DCHECK(firebase_watcher_set_ && watch_callback_);
  FTL_NOTIMPLEMENTED();
}

void CloudDeviceSetImpl::OnAuthRevoked(const std::string& reason) {
  if (destruction_sentinel_.DestructedWhile(
          [this] { watch_callback_(Status::NETWORK_ERROR); })) {
    return;
  }
  ResetWatcher();
}

void CloudDeviceSetImpl::OnMalformedEvent() {
  FTL_DCHECK(firebase_watcher_set_ && watch_callback_);
  FTL_NOTIMPLEMENTED();
}

void CloudDeviceSetImpl::OnConnectionError() {
  if (destruction_sentinel_.DestructedWhile(
          [this] { watch_callback_(Status::NETWORK_ERROR); })) {
    return;
  }
  ResetWatcher();
}

void CloudDeviceSetImpl::ResetWatcher() {
  FTL_DCHECK(firebase_watcher_set_ && watch_callback_);
  user_firebase_->UnWatch(this);
  firebase_watcher_set_ = false;
  watch_callback_ = nullptr;
}

}  // namespace cloud_sync
