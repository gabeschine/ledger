// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_CLOUD_SYNC_PUBLIC_AUTH_PROVIDER_H_
#define APPS_LEDGER_SRC_CLOUD_SYNC_PUBLIC_AUTH_PROVIDER_H_

#include <functional>
#include <string>

#include "apps/ledger/src/callback/cancellable.h"
#include "lib/ftl/macros.h"

namespace cloud_sync {

enum class AuthStatus {
  OK,
  // Failed to retrieve the auth token.
  ERROR
};

// Source of tokens that are used to authenticate with cloud services.
//
// Each instance of this class is tied to exactly one user.
class AuthProvider {
 public:
  AuthProvider() {}
  virtual ~AuthProvider() {}

  // Retrieves the Firebase ID token suitable to use with Firebase Real-time
  // Database and Firebase Storage.
  virtual ftl::RefPtr<callback::Cancellable> GetFirebaseToken(
      std::function<void(AuthStatus, std::string)> callback) = 0;

  // Retrieves the Firebase user ID of the user.
  virtual ftl::RefPtr<callback::Cancellable> GetFirebaseUserId(
      std::function<void(AuthStatus, std::string)> callback) = 0;

 private:
  FTL_DISALLOW_COPY_AND_ASSIGN(AuthProvider);
};

}  // namespace cloud_sync

#endif  // APPS_LEDGER_SRC_CLOUD_SYNC_PUBLIC_AUTH_PROVIDER_H_
