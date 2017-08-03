// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_CLOUD_PROVIDER_PUBLIC_RECORD_H_
#define APPS_LEDGER_SRC_CLOUD_PROVIDER_PUBLIC_RECORD_H_

#include "apps/ledger/src/cloud_provider/public/commit.h"
#include "lib/ftl/macros.h"

namespace cloud_provider {

// Represents a commit received from the cloud, along with its server-side
// timestamp and batch position.
struct Record {
  Record();
  Record(Commit n, std::string t, int position = 0, int size = 1);

  ~Record();

  Record(Record&& other);
  Record& operator=(Record&& other);

  Commit commit;
  std::string timestamp;
  size_t batch_position;
  size_t batch_size;

 private:
  FTL_DISALLOW_COPY_AND_ASSIGN(Record);
};

}  // namespace cloud_provider

#endif  // APPS_LEDGER_SRC_CLOUD_PROVIDER_PUBLIC_RECORD_H_
