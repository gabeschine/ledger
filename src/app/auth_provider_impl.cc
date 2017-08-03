// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/app/auth_provider_impl.h"

#include <utility>

#include "apps/ledger/src/callback/cancellable_helper.h"
#include "lib/ftl/functional/make_copyable.h"

namespace ledger {

AuthProviderImpl::AuthProviderImpl(
    ftl::RefPtr<ftl::TaskRunner> task_runner,
    std::string api_key,
    modular::auth::TokenProviderPtr token_provider,
    std::unique_ptr<backoff::Backoff> backoff)
    : task_runner_(std::move(task_runner)),
      api_key_(std::move(api_key)),
      token_provider_(std::move(token_provider)),
      backoff_(std::move(backoff)),
      weak_factory_(this) {}

ftl::RefPtr<callback::Cancellable> AuthProviderImpl::GetFirebaseToken(
    std::function<void(cloud_sync::AuthStatus, std::string)> callback) {
  if (api_key_.empty()) {
    FTL_LOG(WARNING) << "No Firebase API key provided. Connection to Firebase "
                        "may be unauthenticated.";
  }
  auto cancellable = callback::CancellableImpl::Create([] {});
  GetToken([callback = cancellable->WrapCallback(callback)](
      auto status, auto token) { callback(status, token->id_token); });
  return cancellable;
}

ftl::RefPtr<callback::Cancellable> AuthProviderImpl::GetFirebaseUserId(
    std::function<void(cloud_sync::AuthStatus, std::string)> callback) {
  auto cancellable = callback::CancellableImpl::Create([] {});
  GetToken([callback = cancellable->WrapCallback(callback)](
      auto status, auto token) { callback(status, token->local_id); });
  return cancellable;
}

void AuthProviderImpl::GetToken(
    std::function<void(cloud_sync::AuthStatus, modular::auth::FirebaseTokenPtr)>
        callback) {
  token_provider_->GetFirebaseAuthToken(
      api_key_, [ this, callback = std::move(callback) ](
                    modular::auth::FirebaseTokenPtr token,
                    modular::auth::AuthErrPtr error) mutable {
        if (!token || error->status != modular::auth::Status::OK) {
          if (!token) {
            // This should not happen - the token provider returns nullptr when
            // running in the guest mode, but in this case we don't initialize
            // sync and should never call auth provider.
            FTL_LOG(ERROR)
                << "null Firebase token returned from token provider, "
                << "this should never happen. Retrying.";
          } else {
            FTL_LOG(ERROR)
                << "Error retrieving the Firebase token from token provider: "
                << error->status << ", '" << error->message << "', retrying.";
          }
          task_runner_->PostDelayedTask(
              [
                weak_this = weak_factory_.GetWeakPtr(),
                callback = std::move(callback)
              ]() mutable {
                if (weak_this) {
                  weak_this->GetToken(std::move(callback));
                }
              },
              backoff_->GetNext());
          return;
        }

        backoff_->Reset();
        callback(cloud_sync::AuthStatus::OK, std::move(token));
      });
}

}  // namespace ledger
