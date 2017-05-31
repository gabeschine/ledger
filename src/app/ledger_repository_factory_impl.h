// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_APP_LEDGER_REPOSITORY_FACTORY_IMPL_H_
#define APPS_LEDGER_SRC_APP_LEDGER_REPOSITORY_FACTORY_IMPL_H_

#include <memory>
#include <string>

#include "apps/ledger/services/internal/internal.fidl.h"
#include "apps/ledger/src/app/ledger_repository_impl.h"
#include "apps/ledger/src/callback/auto_cleanable.h"
#include "apps/ledger/src/environment/environment.h"
#include "apps/modular/services/auth/token_provider.fidl.h"
#include "lib/ftl/macros.h"

namespace ledger {

class LedgerRepositoryFactoryImpl : public LedgerRepositoryFactory {
 public:
  enum class ConfigPersistence { PERSIST, FORGET };
  explicit LedgerRepositoryFactoryImpl(ledger::Environment* environment,
                                       ConfigPersistence config_persistence);
  ~LedgerRepositoryFactoryImpl() override;

 private:
  // LedgerRepositoryFactory:
  void GetRepository(
      const fidl::String& repository_path,
      const fidl::String& server_id,
      fidl::InterfaceHandle<modular::auth::TokenProvider> token_provider,
      fidl::InterfaceRequest<LedgerRepository> repository_request,
      const GetRepositoryCallback& callback) override;

  ledger::Environment* const environment_;
  const ConfigPersistence config_persistence_;
  callback::AutoCleanableMap<std::string, LedgerRepositoryImpl> repositories_;

  FTL_DISALLOW_COPY_AND_ASSIGN(LedgerRepositoryFactoryImpl);
};

}  // namespace ledger

#endif  // APPS_LEDGER_SRC_APP_LEDGER_REPOSITORY_FACTORY_IMPL_H_
