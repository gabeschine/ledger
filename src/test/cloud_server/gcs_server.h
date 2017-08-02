// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_TEST_CLOUD_SERVER_GCS_SERVER_H_
#define APPS_LEDGER_SRC_TEST_CLOUD_SERVER_GCS_SERVER_H_

#include <functional>
#include <unordered_map>

#include "apps/ledger/src/test/cloud_server/server.h"
#include "apps/network/services/network_service.fidl.h"

namespace ledger {

// Implementation of a google cloud storage server. This implementation is
// partial and only handles the part of the API that the Ledger application
// exercises.
class GcsServer : public Server {
 public:
  GcsServer();
  ~GcsServer() override;

 private:
  void HandleGet(
      network::URLRequestPtr request,
      std::function<void(network::URLResponsePtr)> callback) override;
  void HandlePost(
      network::URLRequestPtr request,
      std::function<void(network::URLResponsePtr)> callback) override;

  std::unordered_map<std::string, std::string> data_;
};

}  // namespace ledger

#endif  // APPS_LEDGER_SRC_TEST_CLOUD_SERVER_GCS_SERVER_H_
