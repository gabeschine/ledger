// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/test/app_test.h"
#include "apps/ledger/src/test/integration/sync/lib.h"

int main(int argc, char** argv) {
  test::integration::sync::ProcessCommandLine(argc, argv);

  return test::TestMain(argc, argv);
}
