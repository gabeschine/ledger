// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_CLOUD_SYNC_IMPL_PATHS_H_
#define APPS_LEDGER_SRC_CLOUD_SYNC_IMPL_PATHS_H_

#include <string>

#include "lib/ftl/strings/string_view.h"

namespace cloud_sync {

// Returns the common object name prefix used for all objects stored on behalf
// of the given user and app.
std::string GetGcsPrefixForApp(ftl::StringView user_id, ftl::StringView app_id);

// Returns the common object name prefix used for all objects stored for the
// given page, based on the prefix for the app.
std::string GetGcsPrefixForPage(ftl::StringView app_prefix,
                                ftl::StringView app_id);

// Returns the Firebase path under which the data for the given user is stored.
std::string GetFirebasePathForUser(ftl::StringView user_id);

// Returns the Firebase path under which the data for the given app is stored.
std::string GetFirebasePathForApp(ftl::StringView user_id,
                                  ftl::StringView app_id);

// Returns the Firebase path under which the data for the given page is stored,
// given the path for the app.
std::string GetFirebasePathForPage(ftl::StringView app_path,
                                   ftl::StringView page_id);

}  // namespace cloud_sync

#endif  // APPS_LEDGER_SRC_CLOUD_SYNC_IMPL_PATHS_H_
