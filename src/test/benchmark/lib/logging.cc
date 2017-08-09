// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/test/benchmark/lib/logging.h"

#include "lib/mtl/tasks/message_loop.h"

namespace test {
namespace benchmark {

bool QuitOnError(ledger::Status status, ftl::StringView description) {
  if (status != ledger::Status::OK) {
    FTL_LOG(ERROR) << description << " failed";
    mtl::MessageLoop::GetCurrent()->PostQuitTask();
    return true;
  }
  return false;
}

std::function<void(ledger::Status)> QuitOnErrorCallback(
    std::string description) {
  return [description](ledger::Status status) {
    QuitOnError(status, description);
  };
}

}  // namespace benchmark
}  // namespace test
