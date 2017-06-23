// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_CLOUD_SYNC_IMPL_CONSTANTS_H_
#define APPS_LEDGER_SRC_CLOUD_SYNC_IMPL_CONSTANTS_H_

#include "lib/ftl/strings/string_view.h"

namespace cloud_sync {

// Key for the timestamp metadata in the SyncMetadata KV store.
constexpr ftl::StringView kTimestampKey = "timestamp";

}  // namespace cloud_sync

#endif  // APPS_LEDGER_SRC_CLOUD_SYNC_IMPL_CONSTANTS_H_
