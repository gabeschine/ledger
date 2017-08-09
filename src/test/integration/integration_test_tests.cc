// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <utility>
#include <vector>

#include "apps/ledger/services/public/ledger.fidl.h"
#include "apps/ledger/src/convert/convert.h"
#include "apps/ledger/src/test/integration/integration_test.h"
#include "apps/ledger/src/test/integration/test_utils.h"
#include "gtest/gtest.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/strings/string_printf.h"
#include "lib/mtl/tasks/message_loop.h"

namespace ledger {
namespace integration_tests {
namespace {

class IntegrationTestTests : public IntegrationTest {};

TEST_F(IntegrationTestTests, MultipleLedgerAppInstances) {
  auto instance1 = NewLedgerAppInstance();
  auto instance2 = NewLedgerAppInstance();

  EXPECT_TRUE(instance1->GetTestLedger());
  EXPECT_TRUE(instance2->GetTestLedger());
}

}  // namespace
}  // namespace integration_tests
}  // namespace ledger
