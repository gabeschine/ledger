// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/network/no_network_service.h"

#include "apps/ledger/src/callback/cancellable_helper.h"

namespace ledger {

NoNetworkService::NoNetworkService(ftl::RefPtr<ftl::TaskRunner> task_runner)
    : task_runner_(task_runner) {}

NoNetworkService::~NoNetworkService() {}

ftl::RefPtr<callback::Cancellable> NoNetworkService::Request(
    std::function<network::URLRequestPtr()> request_factory,
    std::function<void(network::URLResponsePtr)> callback) {
  auto cancellable = callback::CancellableImpl::Create([] {});

  task_runner_->PostTask([callback = cancellable->WrapCallback(callback)] {
    network::URLResponsePtr response = network::URLResponse::New();
    response->error = network::NetworkError::New();
    response->error->code = 1;
    callback(std::move(response));
  });
  return cancellable;
}

}  // namespace ledger
