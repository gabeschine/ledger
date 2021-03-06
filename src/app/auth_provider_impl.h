// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_APP_AUTH_PROVIDER_IMPL_H_
#define APPS_LEDGER_SRC_APP_AUTH_PROVIDER_IMPL_H_

#include <functional>
#include <memory>
#include <string>

#include "apps/ledger/src/backoff/backoff.h"
#include "apps/ledger/src/cloud_sync/public/auth_provider.h"
#include "apps/modular/services/auth/token_provider.fidl.h"
#include "lib/ftl/memory/weak_ptr.h"
#include "lib/ftl/tasks/task_runner.h"

namespace ledger {

// Source of the auth information for cloud sync to use, implemented using the
// system token provider.
//
// If configured with an empty |api_key|, doesn't attempt to use
// |token_provider| and yields empty Firebase tokens and user ids. This allows
// the code to work without auth against public instances (e.g. for running
// benchmarks).
//
// This is currently a placeholder that always returns an empty token.
class AuthProviderImpl : public cloud_sync::AuthProvider {
 public:
  AuthProviderImpl(ftl::RefPtr<ftl::TaskRunner> task_runner,
                   std::string api_key,
                   modular::auth::TokenProviderPtr token_provider,
                   std::unique_ptr<backoff::Backoff> backoff);

  // AuthProvider:
  ftl::RefPtr<callback::Cancellable> GetFirebaseToken(
      std::function<void(cloud_sync::AuthStatus, std::string)> callback)
      override;

  ftl::RefPtr<callback::Cancellable> GetFirebaseUserId(
      std::function<void(cloud_sync::AuthStatus, std::string)> callback)
      override;

 private:
  // Retrieves the Firebase token from the token provider, transparently
  // retrying the request until success.
  void GetToken(std::function<void(cloud_sync::AuthStatus,
                                   modular::auth::FirebaseTokenPtr)> callback);

  ftl::RefPtr<ftl::TaskRunner> task_runner_;
  const std::string api_key_;
  modular::auth::TokenProviderPtr token_provider_;
  const std::unique_ptr<backoff::Backoff> backoff_;

  // Must be the last member field.
  ftl::WeakPtrFactory<AuthProviderImpl> weak_factory_;
};

}  // namespace ledger

#endif  // APPS_LEDGER_SRC_APP_AUTH_PROVIDER_IMPL_H_
