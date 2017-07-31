// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_APP_LEDGER_IMPL_H_
#define APPS_LEDGER_SRC_APP_LEDGER_IMPL_H_

#include <memory>

#include "apps/ledger/services/public/ledger.fidl.h"
#include "apps/ledger/src/app/page_manager.h"
#include "apps/ledger/src/convert/convert.h"
#include "apps/ledger/src/storage/public/ledger_storage.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/strings/string_view.h"

namespace ledger {

// An implementation of the |Ledger| FIDL interface.
class LedgerImpl : public Ledger {
 public:
  // Delegate capable of actually performing the page operations.
  class Delegate {
   public:
    Delegate() {}
    ~Delegate() {}

    virtual void GetPage(convert::ExtendedStringView page_id,
                         fidl::InterfaceRequest<Page> page_request,
                         std::function<void(Status)> callback) = 0;

    virtual Status DeletePage(convert::ExtendedStringView page_id) = 0;

    virtual void SetConflictResolverFactory(
        fidl::InterfaceHandle<ConflictResolverFactory> factory) = 0;

   private:
    FTL_DISALLOW_COPY_AND_ASSIGN(Delegate);
  };

  // |delegate| outlives LedgerImpl.
  explicit LedgerImpl(Delegate* delegate);
  ~LedgerImpl() override;

 private:
  // Ledger:
  void GetRootPage(fidl::InterfaceRequest<Page> page_request,
                   const GetRootPageCallback& callback) override;
  void GetPage(fidl::Array<uint8_t> id,
               fidl::InterfaceRequest<Page> page_request,
               const GetPageCallback& callback) override;
  void DeletePage(fidl::Array<uint8_t> id,
                  const DeletePageCallback& callback) override;

  void SetConflictResolverFactory(
      fidl::InterfaceHandle<ConflictResolverFactory> factory,
      const SetConflictResolverFactoryCallback& callback) override;

  Delegate* const delegate_;

  FTL_DISALLOW_COPY_AND_ASSIGN(LedgerImpl);
};
}  // namespace ledger

#endif  // APPS_LEDGER_SRC_APP_LEDGER_IMPL_H_
