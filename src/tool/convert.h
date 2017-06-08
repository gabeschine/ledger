// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_TOOL_CONVERT_H_
#define APPS_LEDGER_SRC_TOOL_CONVERT_H_

#include <string>

#include "lib/ftl/strings/string_view.h"

namespace tool {

// Inverse of the transformation currently used by DeviceRunner to translate
// human-readable username to user ID.
bool FromHexString(ftl::StringView hex_string, std::string* result);

}  // namespace tool

#endif  // APPS_LEDGER_SRC_TOOL_CONVERT_H_
