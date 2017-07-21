// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_NETWORK_FAKE_NETWORK_SERVICE_H_
#define APPS_LEDGER_SRC_NETWORK_FAKE_NETWORK_SERVICE_H_

#include "apps/ledger/src/network/network_service.h"
#include "apps/network/services/network_service.fidl.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/tasks/task_runner.h"

namespace ledger {

class FakeNetworkService : public NetworkService {
 public:
  FakeNetworkService(ftl::RefPtr<ftl::TaskRunner> task_runner);
  ~FakeNetworkService() override;

  network::URLRequest* GetRequest();
  void ResetRequest();

  void SetResponse(network::URLResponsePtr response);

  void SetSocketResponse(mx::socket body, uint32_t status_code);

  void SetStringResponse(const std::string& body, uint32_t status_code);

 private:
  // NetworkService
  ftl::RefPtr<callback::Cancellable> Request(
      std::function<network::URLRequestPtr()> request_factory,
      std::function<void(network::URLResponsePtr)> callback) override;

  network::URLRequestPtr request_received_;
  network::URLResponsePtr response_to_return_;
  ftl::RefPtr<ftl::TaskRunner> task_runner_;

  FTL_DISALLOW_COPY_AND_ASSIGN(FakeNetworkService);
};

}  // namespace ledger

#endif  // APPS_LEDGER_SRC_NETWORK_FAKE_NETWORK_SERVICE_H_
