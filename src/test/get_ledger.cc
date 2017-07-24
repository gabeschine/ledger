// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/test/get_ledger.h"

#include <utility>

#include "application/lib/app/connect.h"
#include "apps/ledger/services/internal/internal.fidl.h"
#include "apps/ledger/src/callback/capture.h"
#include "apps/ledger/src/convert/convert.h"
#include "apps/ledger/src/test/test_with_message_loop.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/logging.h"
#include "lib/mtl/tasks/message_loop.h"

namespace test {
namespace {
constexpr ftl::TimeDelta kTimeout = ftl::TimeDelta::FromSeconds(10);
}  // namespace

ledger::Status GetLedger(
    mtl::MessageLoop* loop,
    app::ApplicationContext* context,
    app::ApplicationControllerPtr* controller,
    ledger::fidl_helpers::SetBoundable<modular::auth::TokenProvider>*
        token_provider_impl,
    std::string ledger_name,
    std::string ledger_repository_path,
    SyncState sync,
    std::string server_id,
    ledger::LedgerPtr* ledger_ptr,
    Erase erase) {
  ledger::LedgerRepositoryFactoryPtr repository_factory;
  app::ServiceProviderPtr child_services;
  auto launch_info = app::ApplicationLaunchInfo::New();
  launch_info->url = "ledger";
  launch_info->services = child_services.NewRequest();
  launch_info->arguments.push_back("--no_minfs_wait");
  launch_info->arguments.push_back("--no_persisted_config");
  launch_info->arguments.push_back("--no_statistics_reporting_for_testing");

  context->launcher()->CreateApplication(std::move(launch_info),
                                         controller->NewRequest());
  app::ConnectToService(child_services.get(), repository_factory.NewRequest());
  ledger::LedgerRepositoryPtr repository;
  ledger::FirebaseConfigPtr firebase_config;
  if (sync == SyncState::CLOUD_SYNC_ENABLED) {
    firebase_config = ledger::FirebaseConfig::New();
    firebase_config->server_id = server_id;
    firebase_config->api_key = "";
  }

  auto quitCallback = [&loop] { loop->PostQuitTask(); };

  ledger::Status status = ledger::Status::UNKNOWN_ERROR;
  if (erase == Erase::ERASE_CLOUD) {
    modular::auth::TokenProviderPtr token_provider_ptr;
    token_provider_impl->AddBinding(token_provider_ptr.NewRequest());
    repository_factory->EraseRepository(
        ledger_repository_path, firebase_config.Clone(),
        std::move(token_provider_ptr),
        callback::Capture(quitCallback, &status));
    if (RunGivenLoopWithTimeout(loop, kTimeout) ||
        status != ledger::Status::OK) {
      FTL_LOG(ERROR) << "Unable to erase repository.";
      return ledger::Status::INTERNAL_ERROR;
    }
  }

  modular::auth::TokenProviderPtr token_provider_ptr;
  token_provider_impl->AddBinding(token_provider_ptr.NewRequest());
  repository_factory->GetRepository(
      ledger_repository_path, std::move(firebase_config),
      std::move(token_provider_ptr), repository.NewRequest(),
      callback::Capture(quitCallback, &status));
  if (RunGivenLoopWithTimeout(loop, kTimeout)) {
    FTL_LOG(ERROR) << "Unable to get repository.";
    return ledger::Status::INTERNAL_ERROR;
  }
  if (status != ledger::Status::OK) {
    FTL_LOG(ERROR) << "Failure while getting repository.";
    return status;
  }

  repository->GetLedger(convert::ToArray(ledger_name), ledger_ptr->NewRequest(),
                        callback::Capture(quitCallback, &status));
  if (RunGivenLoopWithTimeout(loop, kTimeout)) {
    FTL_LOG(ERROR) << "Unable to get ledger.";
    return ledger::Status::INTERNAL_ERROR;
  }
  if (status != ledger::Status::OK) {
    FTL_LOG(ERROR) << "Failure while getting ledger.";
    return status;
  }
  ledger_ptr->set_connection_error_handler([&loop] {
    FTL_LOG(ERROR) << "The ledger connection was closed, quitting.";
    loop->PostQuitTask();
  });

  return status;
}

ledger::Status GetPageEnsureInitialized(mtl::MessageLoop* loop,
                                        ledger::LedgerPtr* ledger,
                                        fidl::Array<uint8_t> requested_id,
                                        ledger::PagePtr* page,
                                        fidl::Array<uint8_t>* page_id) {
  ledger::Status status;
  (*ledger)->GetPage(
      std::move(requested_id), page->NewRequest(),
      callback::Capture([loop] { loop->PostQuitTask(); }, &status));
  if (RunGivenLoopWithTimeout(loop, kTimeout)) {
    FTL_LOG(ERROR) << "Unable to get page.";
    return ledger::Status::INTERNAL_ERROR;
  }
  if (status != ledger::Status::OK) {
    return status;
  }

  page->set_connection_error_handler([loop] {
    FTL_LOG(ERROR) << "The page connection was closed, quitting.";
    loop->PostQuitTask();
  });

  (*page)->GetId(callback::Capture([loop] { loop->PostQuitTask(); }, page_id));
  RunGivenLoopWithTimeout(loop, kTimeout);
  return status;
}

}  // namespace test
