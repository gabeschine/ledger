// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/storage/public/object.h"

#include "lib/mtl/vmo/strings.h"

namespace storage {

Status Object::GetVmo(mx::vmo* vmo) const {
  ftl::StringView data;
  Status status = GetData(&data);
  if (status != Status::OK) {
    return status;
  }

  if (!mtl::VmoFromString(data, vmo)) {
    return Status::INTERNAL_IO_ERROR;
  }

  return Status::OK;
}

}  // namespace storage
