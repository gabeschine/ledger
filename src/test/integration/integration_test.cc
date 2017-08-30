// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/test/integration/integration_test.h"

#include <thread>

#include "apps/ledger/src/app/erase_remote_repository_operation.h"
#include "apps/ledger/src/app/ledger_repository_factory_impl.h"
#include "apps/ledger/src/callback/synchronous_task.h"
#include "apps/ledger/src/glue/socket/socket_pair.h"
#include "apps/ledger/src/glue/socket/socket_writer.h"
#include "apps/ledger/src/network/network_service_impl.h"
#include "apps/ledger/src/test/fake_token_provider.h"
#include "apps/ledger/src/test/integration/test_utils.h"
#include "lib/fidl/cpp/bindings/binding_set.h"
#include "lib/ftl/files/scoped_temp_dir.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/mtl/socket/strings.h"
#include "lib/mtl/tasks/message_loop.h"
#include "lib/mtl/threading/create_thread.h"

namespace test {
namespace integration {
void IntegrationTest::SetUp() {
  ::testing::Test::SetUp();
  socket_thread_ = mtl::CreateThread(&socket_task_runner_);
  app_factory_ = GetLedgerAppInstanceFactory();
}

void IntegrationTest::TearDown() {
  socket_task_runner_->PostTask(
      [] { mtl::MessageLoop::GetCurrent()->QuitNow(); });
  socket_thread_.join();

  ::testing::Test::TearDown();
}

mx::socket IntegrationTest::StreamDataToSocket(std::string data) {
  glue::SocketPair sockets;
  socket_task_runner_->PostTask(ftl::MakeCopyable([
    socket = std::move(sockets.socket1), data = std::move(data)
  ]() mutable {
    auto writer = new glue::StringSocketWriter();
    writer->Start(std::move(data), std::move(socket));
  }));
  return std::move(sockets.socket2);
}

std::unique_ptr<LedgerAppInstanceFactory::LedgerAppInstance>
IntegrationTest::NewLedgerAppInstance() {
  return app_factory_->NewLedgerAppInstance();
}

}  // namespace integration
}  // namespace test
