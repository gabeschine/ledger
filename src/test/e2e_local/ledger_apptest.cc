// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string.h>

#include "application/lib/app/connect.h"
#include "application/services/application_environment.fidl.h"
#include "apps/ledger/services/internal/internal.fidl-sync.h"
#include "apps/ledger/services/internal/internal.fidl.h"
#include "apps/ledger/services/public/ledger.fidl-sync.h"
#include "apps/ledger/services/public/ledger.fidl.h"
#include "apps/ledger/src/callback/capture.h"
#include "apps/ledger/src/test/app_test.h"
#include "apps/ledger/src/test/fake_token_provider.h"
#include "gtest/gtest.h"
#include "lib/fidl/cpp/bindings/binding_set.h"
#include "lib/fidl/cpp/bindings/synchronous_interface_ptr.h"
#include "lib/ftl/files/directory.h"
#include "lib/ftl/files/file.h"
#include "lib/ftl/files/scoped_temp_dir.h"
#include "lib/mtl/tasks/message_loop.h"
#include "lib/mtl/vmo/strings.h"

namespace test {
namespace e2e_local {
namespace {

template <class A>
bool Equals(const fidl::Array<uint8_t>& a1, const A& a2) {
  if (a1.size() != a2.size())
    return false;
  return memcmp(a1.data(), a2.data(), a1.size()) == 0;
}

fidl::Array<uint8_t> TestArray() {
  std::string value = "value";
  fidl::Array<uint8_t> result = fidl::Array<uint8_t>::New(value.size());
  memcpy(&result[0], &value[0], value.size());
  return result;
}

class LedgerAppTest : public ::test::TestWithMessageLoop {
 public:
  LedgerAppTest()
      : application_context_(
            app::ApplicationContext::CreateFromStartupInfoNotChecked()) {}
  ~LedgerAppTest() override {}

 protected:
  void Init(std::vector<std::string> additional_args) {
    app::ServiceProviderPtr child_services;
    auto launch_info = app::ApplicationLaunchInfo::New();
    launch_info->url = "ledger";
    launch_info->services = child_services.NewRequest();
    launch_info->arguments.push_back("--no_minfs_wait");
    launch_info->arguments.push_back("--no_persisted_config");
    launch_info->arguments.push_back("--no_statistics_reporting_for_testing");
    for (auto& additional_arg : additional_args) {
      launch_info->arguments.push_back(additional_arg);
    }
    application_context()->launcher()->CreateApplication(
        std::move(launch_info), ledger_controller_.NewRequest());

    ledger_controller_.set_connection_error_handler([this] {
      for (const auto& callback : ledger_shutdown_callbacks_) {
        callback();
      }
    });

    app::ConnectToService(child_services.get(),
                          ledger_repository_factory_.NewRequest());
    app::ConnectToService(child_services.get(),
                          fidl::GetSynchronousProxy(&controller_));
  }

  void RegisterShutdownCallback(std::function<void()> callback) {
    ledger_shutdown_callbacks_.push_back(std::move(callback));
  }

  app::ApplicationContext* application_context() {
    return application_context_.get();
  }

 private:
  app::ApplicationControllerPtr ledger_controller_;
  std::vector<std::function<void()>> ledger_shutdown_callbacks_;
  std::unique_ptr<app::ApplicationContext> application_context_;

 protected:
  ledger::LedgerRepositoryFactoryPtr ledger_repository_factory_;
  fidl::SynchronousInterfacePtr<ledger::Ledger> ledger_;
  fidl::SynchronousInterfacePtr<ledger::LedgerController> controller_;
};

TEST_F(LedgerAppTest, PutAndGet) {
  Init({});
  ledger::Status status;
  fidl::SynchronousInterfacePtr<ledger::LedgerRepository> ledger_repository;
  files::ScopedTempDir tmp_dir;
  ledger_repository_factory_->GetRepository(
      tmp_dir.path(), nullptr, nullptr,
      fidl::GetSynchronousProxy(&ledger_repository),
      callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(ledger::Status::OK, status);

  ledger_repository->GetLedger(TestArray(), fidl::GetSynchronousProxy(&ledger_),
                               &status);
  ASSERT_EQ(ledger::Status::OK, status);

  fidl::SynchronousInterfacePtr<ledger::Page> page;
  ledger_->GetRootPage(fidl::GetSynchronousProxy(&page), &status);
  ASSERT_EQ(ledger::Status::OK, status);
  page->Put(TestArray(), TestArray(), &status);
  EXPECT_EQ(ledger::Status::OK, status);
  fidl::SynchronousInterfacePtr<ledger::PageSnapshot> snapshot;
  page->GetSnapshot(GetSynchronousProxy(&snapshot), nullptr, nullptr, &status);
  EXPECT_EQ(ledger::Status::OK, status);
  mx::vmo value;
  snapshot->Get(TestArray(), &status, &value);
  EXPECT_EQ(ledger::Status::OK, status);
  std::string value_as_string;
  EXPECT_TRUE(mtl::StringFromVmo(value, &value_as_string));
  EXPECT_TRUE(Equals(TestArray(), value_as_string));
}

TEST_F(LedgerAppTest, Terminate) {
  Init({});
  bool called = false;
  RegisterShutdownCallback([this, &called] {
    called = true;
    message_loop_.PostQuitTask();
  });
  controller_->Terminate();
  RunLoopWithTimeout();
  EXPECT_TRUE(called);
}

// Triggers the cloud erased recovery codepath and verifies that:
//  - Ledger disconnects the clients
//  - the repository directory is cleared
TEST_F(LedgerAppTest, CloudErasedRecovery) {
  Init({"--no_network_for_testing", "--trigger_cloud_erased_for_testing"});
  bool ledger_shut_down = false;
  RegisterShutdownCallback([&ledger_shut_down] { ledger_shut_down = true; });

  ledger::Status status;
  ledger::LedgerRepositoryPtr ledger_repository;
  files::ScopedTempDir tmp_dir;
  std::string content_path = tmp_dir.path() + "/content";
  std::string deletion_sentinel_path = content_path + "/sentinel";
  ASSERT_TRUE(files::CreateDirectory(content_path));
  ASSERT_TRUE(files::WriteFile(deletion_sentinel_path, "", 0));
  ASSERT_TRUE(files::IsFile(deletion_sentinel_path));

  ledger::FirebaseConfigPtr firebase_config = ledger::FirebaseConfig::New();
  firebase_config->server_id = "network_is_disabled_anyway";
  firebase_config->api_key = "abc";
  test::FakeTokenProvider token_provider("id_token", "local_id", "email",
                                         "client_id");
  modular::auth::TokenProviderPtr token_provider_ptr;
  fidl::BindingSet<modular::auth::TokenProvider> token_provider_bindings;
  token_provider_bindings.AddBinding(&token_provider,
                                     token_provider_ptr.NewRequest());
  ledger_repository_factory_->GetRepository(
      tmp_dir.path(), std::move(firebase_config), std::move(token_provider_ptr),
      ledger_repository.NewRequest(),
      callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(ledger::Status::OK, status);

  bool repo_disconnected = false;
  ledger_repository.set_connection_error_handler(
      [&repo_disconnected] { repo_disconnected = true; });

  // Run the message loop until Ledger clears the repo directory and disconnects
  // the client.
  bool cleared = RunLoopUntil([deletion_sentinel_path, &repo_disconnected] {
    return !files::IsFile(deletion_sentinel_path) && repo_disconnected;
  });
  EXPECT_FALSE(files::IsFile(deletion_sentinel_path));
  EXPECT_TRUE(repo_disconnected);
  EXPECT_TRUE(cleared);

  // Verify that the Ledger app didn't crash.
  EXPECT_FALSE(ledger_shut_down);
}

TEST_F(LedgerAppTest, EraseRepository) {
  Init({"--no_network_for_testing"});
  bool ledger_shut_down = false;
  RegisterShutdownCallback([&ledger_shut_down] { ledger_shut_down = true; });

  ledger::Status status;
  files::ScopedTempDir tmp_dir;
  std::string content_path = tmp_dir.path() + "/content";
  std::string deletion_sentinel_path = content_path + "/sentinel";
  ASSERT_TRUE(files::CreateDirectory(content_path));
  ASSERT_TRUE(files::WriteFile(deletion_sentinel_path, "", 0));
  ASSERT_TRUE(files::IsFile(deletion_sentinel_path));

  ledger::FirebaseConfigPtr firebase_config = ledger::FirebaseConfig::New();
  firebase_config->server_id = "network_is_disabled_anyway";
  firebase_config->api_key = "abc";
  test::FakeTokenProvider token_provider("id_token", "local_id", "email",
                                         "client_id");
  fidl::BindingSet<modular::auth::TokenProvider> token_provider_bindings;

  // Connect to the repository, so that we can verify that we're disconnected
  // when the erase method is called.
  ledger::LedgerRepositoryPtr ledger_repository;
  modular::auth::TokenProviderPtr token_provider_ptr_1;
  token_provider_bindings.AddBinding(&token_provider,
                                     token_provider_ptr_1.NewRequest());
  ledger_repository_factory_->GetRepository(
      tmp_dir.path(), firebase_config.Clone(), std::move(token_provider_ptr_1),
      ledger_repository.NewRequest(),
      callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(ledger::Status::OK, status);

  bool repo_disconnected = false;
  ledger_repository.set_connection_error_handler(
      [&repo_disconnected] { repo_disconnected = true; });

  // Erase the repository - this is expected to fail as network is disabled for
  // this test, but it should still erase the local storage and disconnect the
  // client.
  modular::auth::TokenProviderPtr token_provider_ptr_2;
  token_provider_bindings.AddBinding(&token_provider,
                                     token_provider_ptr_2.NewRequest());
  ledger_repository_factory_->EraseRepository(
      tmp_dir.path(), firebase_config.Clone(), std::move(token_provider_ptr_2),
      callback::Capture(MakeQuitTask(), &status));
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_EQ(ledger::Status::INTERNAL_ERROR, status);

  // Verify that the local storage was cleared and the client was disconnected.
  bool cleared = RunLoopUntil([deletion_sentinel_path, &repo_disconnected] {
    return !files::IsFile(deletion_sentinel_path) && repo_disconnected;
  });
  EXPECT_FALSE(files::IsFile(deletion_sentinel_path));
  EXPECT_TRUE(repo_disconnected);
  EXPECT_TRUE(cleared);

  // Verify that the Ledger app didn't crash.
  EXPECT_FALSE(ledger_shut_down);
}

}  // namespace
}  // namespace e2e_local
}  // namespace test
