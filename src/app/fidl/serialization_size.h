// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_SRC_APP_FIDL_SERIALIZATION_SIZE_H_
#define APPS_LEDGER_SRC_APP_FIDL_SERIALIZATION_SIZE_H_

#include <magenta/types.h>
#include <stddef.h>

#include "apps/ledger/services/public/ledger.fidl.h"

namespace ledger {
namespace fidl_serialization {

// Maximal size of data that will be returned inline.
constexpr size_t kMaxInlineDataSize = MX_CHANNEL_MAX_MSG_BYTES * 9 / 10;
constexpr size_t kMaxMessageHandles = MX_CHANNEL_MAX_MSG_HANDLES;

const size_t kArrayHeaderSize = sizeof(fidl::internal::Array_Data<char>);
const size_t kPointerSize = sizeof(uint64_t);
const size_t kEnumSize = sizeof(int32_t);
const size_t kHandleSize = sizeof(int32_t);

// The overhead for storing the pointer, the timestamp (int64) and the two
// arrays.
constexpr size_t kPageChangeHeaderSize =
    kPointerSize + sizeof(int64_t) + 2 * kArrayHeaderSize;

// Returns the fidl size of a byte array with the given length.
size_t GetByteArraySize(size_t array_length);

// Returns the fidl size of an Entry holding a key with the given length.
size_t GetEntrySize(size_t key_length);

}  // namespace fidl_serialization
}  //  namespace ledger

#endif  // APPS_LEDGER_SRC_APP_FIDL_SERIALIZATION_SIZE_H_
